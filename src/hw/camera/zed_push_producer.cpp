/**
 * @file zed_push_producer.cpp
 * @brief Implementation of ZedPushProducer
 *
 * Grab loop design:
 *   1. sl::Camera::grab() blocks until a new frame pair is ready (or timeout).
 *   2. Color is retrieved via VIEW::LEFT_BGR (SDK 5.x, U8_C3) — this gives a
 *      3-channel BGR image directly, avoiding a BGRA→BGR strip on the CPU.
 *   3. When depth is enabled, MEASURE::DEPTH (F32_C1, metres) is retrieved,
 *      sanitized (NaN/Inf → 0 via cv::patchNaNs + clamping), then quantized
 *      to CV_16UC1 (millimetres) for downstream compatibility with the
 *      RealSense depth pipeline.
 *
 * Thread safety: the sl::Camera handle is used exclusively by this thread
 * once start() is called.  ZedCameraComponent must not call grab() concurrently.
 */

#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "opencv2/core.hpp"
#include "opencv2/imgproc.hpp"

#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/camera/zed_camera_component.hpp"
#include "trossen_sdk/hw/camera/zed_push_producer.hpp"
#include "trossen_sdk/runtime/push_producer_registry.hpp"

namespace trossen::hw::camera {

/// Maximum valid depth value (millimetres) when quantizing F32 → U16.
/// Matches std::numeric_limits<uint16_t>::max() (65 535 mm ≈ 65.5 m), which
/// exceeds every ZED model's maximum range (~20 m for ZED 2).  Values above
/// this are clamped so the subsequent convertTo(CV_16UC1) cannot overflow.
static constexpr float kDepthU16MaxMm =
  static_cast<float>(std::numeric_limits<uint16_t>::max());

// ─────────────────────────────────────────────────────────────
// Metadata helpers
// ─────────────────────────────────────────────────────────────

nlohmann::ordered_json ZedPushProducer::ZedPushProducerMetadata::get_info() const {
  nlohmann::ordered_json features;

  // Color stream feature
  nlohmann::ordered_json color_feature;
  color_feature["id"] = id;
  color_feature["dtype"] = "video";
  color_feature["shape"] = {height, width, channels};
  color_feature["names"] = {"height", "width", "channels"};
  // TODO(shantanuparab-tr): codec and pix_fmt for color are placeholders;
  // update after validating with LeRobot v2 depth support and alpha user feedback
  color_feature["info"] = {
    {"video.fps", fps},
    {"video.height", height},
    {"video.width", width},
    {"video.channels", channels},
    {"video.codec", codec},
    {"video.pix_fmt", pix_fmt},
    {"video.is_depth_map", false},
    {"has_audio", has_audio}};
  features["observation.images." + id] = color_feature;

  // Depth stream feature (when present)
  if (has_depth) {
    nlohmann::ordered_json depth_feature;
    std::string depth_id = id + "_depth";
    depth_feature["id"] = depth_id;
    depth_feature["dtype"] = "video";
    depth_feature["shape"] = {height, width, 1};
    depth_feature["names"] = {"height", "width", "channels"};
    // TODO(shantanuparab-tr): depth codec ("ffv1") and pix_fmt ("gray16le") are
    // provisional; update after validating with LeRobot v2 depth support and alpha
    // user feedback. ffv1 chosen as lossless codec for metric depth data;
    // gray16le matches 16-bit unsigned depth (little-endian).
    depth_feature["info"] = {
      {"video.fps", fps},
      {"video.height", height},
      {"video.width", width},
      {"video.channels", 1},
      {"video.codec", "ffv1"},
      {"video.pix_fmt", "gray16le"},
      {"video.is_depth_map", true},
      {"has_audio", false}};
    features["observation.images." + depth_id] = depth_feature;
  }

  return features;
}

nlohmann::ordered_json ZedPushProducer::ZedPushProducerMetadata::get_stream_info() const {
  nlohmann::ordered_json info;
  info["cameras"][id] = {
    {"width", width},
    {"height", height},
    {"fps", fps},
    {"channels", channels},
    {"codec", codec},
    {"pix_fmt", pix_fmt},
    {"is_depth_map", false},
    {"has_audio", has_audio}};

  if (has_depth) {
    std::string depth_id = id + "_depth";
    // TODO(shantanuparab-tr): depth codec/pix_fmt — see TODO in get_info()
    info["cameras"][depth_id] = {
      {"width", width},
      {"height", height},
      {"fps", fps},
      {"channels", 1},
      {"codec", "ffv1"},
      {"pix_fmt", "gray16le"},
      {"is_depth_map", true},
      {"has_audio", false}};
  }

  return info;
}

// ─────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────

ZedPushProducer::ZedPushProducer(
  std::shared_ptr<::trossen::hw::HardwareComponent> hardware,
  const nlohmann::json& config)
{
  auto cam = std::dynamic_pointer_cast<ZedCameraComponent>(hardware);
  if (!cam) {
    throw std::runtime_error(
      "ZedPushProducer requires ZedCameraComponent, got: " +
      hardware->get_type());
  }

  camera_ = cam->get_hardware();
  if (!camera_) {
    throw std::runtime_error(
      "ZedPushProducer: ZedCameraComponent has null camera handle");
  }

  use_depth_ = cam->get_use_depth();

  // Parse producer config
  cfg_.stream_id = config.value("stream_id", cam->get_identifier());
  cfg_.color_encoding = config.value("encoding", "bgr8");
  if (cfg_.color_encoding != "bgr8" && cfg_.color_encoding != "rgb8") {
    throw std::runtime_error(
      "[ZedPushProducer] Unsupported color encoding: " + cfg_.color_encoding +
      ". Valid: bgr8, rgb8");
  }
  cfg_.use_device_time = config.value("use_device_time", true);
  cfg_.timeout_ms = config.value("timeout_ms", 3000);
  if (cfg_.timeout_ms <= 0) {
    cfg_.timeout_ms = 3000;
  }
  cfg_.fps = config.value("fps", 30);
  cfg_.remove_saturated_areas = config.value("remove_saturated_areas", false);

  // Populate metadata
  metadata_.type = "zed_camera";
  metadata_.id = cfg_.stream_id;
  metadata_.name = cfg_.stream_id;
  metadata_.description =
    "Unified push producer for ZED camera (color + optional depth)";
  metadata_.width = cam->get_width();
  metadata_.height = cam->get_height();
  metadata_.fps = cam->get_fps();
  // TODO(shantanuparab-tr): codec and pix_fmt are placeholders; update after
  // validating with LeRobot v2 depth support and alpha user feedback
  metadata_.codec = "av1";
  metadata_.pix_fmt = "yuv420p";
  metadata_.channels = 3;
  metadata_.has_audio = false;
  metadata_.has_depth = use_depth_;

  std::cout << "[ZedPushProducer] Constructed for stream '"
            << cfg_.stream_id << "' (depth=" << (use_depth_ ? "on" : "off")
            << ", remove_saturated=" << (cfg_.remove_saturated_areas ? "on" : "off")
            << ")" << std::endl;
}

ZedPushProducer::~ZedPushProducer() {
  stop();
}

// ─────────────────────────────────────────────────────────────
// Start / Stop
// ─────────────────────────────────────────────────────────────

bool ZedPushProducer::start(
  const std::function<void(std::shared_ptr<data::RecordBase>)>& emit)
{
  if (running_.load()) {
    std::cerr << "[ZedPushProducer] Already running." << std::endl;
    return false;
  }

  running_.store(true);
  push_thread_ = std::thread([this, emit]() { push_loop(emit); });

  std::cout << "[ZedPushProducer] Started push thread for stream '"
            << cfg_.stream_id << "' (depth=" << (use_depth_ ? "on" : "off") << ")"
            << std::endl;
  return true;
}

void ZedPushProducer::stop() {
  if (!running_.load()) {
    return;
  }

  running_.store(false);

  if (push_thread_.joinable()) {
    push_thread_.join();
  }

  std::cout << "[ZedPushProducer] Stopped push thread for stream '"
            << cfg_.stream_id << "'. produced=" << stats_.produced
            << " dropped=" << stats_.dropped << std::endl;
}

// ─────────────────────────────────────────────────────────────
// Grab loop
// ─────────────────────────────────────────────────────────────

void ZedPushProducer::push_loop(
  const std::function<void(std::shared_ptr<data::RecordBase>)>& emit)
{
  // Build RuntimeParameters once; reused every grab() call
  sl::RuntimeParameters rt;
  rt.remove_saturated_areas = cfg_.remove_saturated_areas;

  while (running_.load(std::memory_order_relaxed)) {
    // grab() blocks until a new frame is ready
    sl::ERROR_CODE grab_err = camera_->grab(rt);
    if (grab_err != sl::ERROR_CODE::SUCCESS) {
      // sl::toString returns an sl::String; wrapping in std::string avoids
      // dangling pointer when the temporary sl::String is destroyed
      std::cerr << "[ZedPushProducer] grab() failed: "
                << std::string(sl::toString(grab_err).c_str()) << std::endl;
      ++stats_.dropped;
      continue;
    }

    // ── Color retrieval ──
    // VIEW::LEFT_BGR is SDK 5.x: returns U8_C3 (BGR) directly on CPU,
    // avoiding the extra BGRA→BGR conversion that VIEW::LEFT would require.
    sl::ERROR_CODE img_err = camera_->retrieveImage(
      sl_color_, sl::VIEW::LEFT_BGR, sl::MEM::CPU);
    if (img_err != sl::ERROR_CODE::SUCCESS) {
      ++stats_.dropped;
      continue;
    }

    const int w = static_cast<int>(sl_color_.getWidth());
    const int h = static_cast<int>(sl_color_.getHeight());

    // Wrap sl::Mat data in a cv::Mat (no copy), then clone to decouple from
    // the SDK's internal buffer which is recycled on the next grab().
    cv::Mat color_wrap(h, w, CV_8UC3,
                       sl_color_.getPtr<sl::uchar3>(sl::MEM::CPU),
                       sl_color_.getStepBytes(sl::MEM::CPU));

    cv::Mat color_out;
    if (cfg_.color_encoding == "bgr8") {
      color_out = color_wrap.clone();
    } else if (cfg_.color_encoding == "rgb8") {
      cv::cvtColor(color_wrap, color_out, cv::COLOR_BGR2RGB);
    } else {
      throw std::runtime_error(
        "[ZedPushProducer] Unsupported color encoding: " + cfg_.color_encoding);
    }

    auto rec = std::make_shared<data::ImageRecord>();
    rec->image = std::move(color_out);
    rec->width = static_cast<uint32_t>(w);
    rec->height = static_cast<uint32_t>(h);
    rec->channels = 3;
    rec->encoding = cfg_.color_encoding;

    // ── Optional depth retrieval ──
    if (use_depth_) {
      sl::ERROR_CODE depth_err = camera_->retrieveMeasure(
        sl_depth_, sl::MEASURE::DEPTH, sl::MEM::CPU);
      if (depth_err == sl::ERROR_CODE::SUCCESS) {
        const int dw = static_cast<int>(sl_depth_.getWidth());
        const int dh = static_cast<int>(sl_depth_.getHeight());

        // sl::MEASURE::DEPTH is F32_C1 in the coordinate unit set at init
        // (we use MILLIMETER).  Wrap without copy.
        cv::Mat depth_f32(dh, dw, CV_32FC1,
                          sl_depth_.getPtr<float>(sl::MEM::CPU),
                          sl_depth_.getStepBytes(sl::MEM::CPU));

        // Sanitize invalid pixels.  The ZED SDK marks invalid depth using:
        //   sl::OCCLUSION_VALUE / sl::INVALID_VALUE  →  NaN
        //   sl::TOO_FAR                               →  +Inf
        //   sl::TOO_CLOSE                             →  -Inf
        cv::Mat depth_clean = depth_f32.clone();
        cv::patchNaNs(depth_clean, 0.0);
        cv::max(depth_clean, 0.0f, depth_clean);
        cv::min(depth_clean, kDepthU16MaxMm, depth_clean);

        cv::Mat depth_u16;
        depth_clean.convertTo(depth_u16, CV_16UC1);

        rec->depth_image = std::move(depth_u16);
        // depth_scale = 0.001: each raw unit is 1 mm, multiply by 0.001 to
        // get metres.
        rec->depth_scale = 0.001f;
      }
      // If depth retrieval fails for this frame, emit color-only record
    }

    // ── Timestamp ──
    uint64_t mono_now = data::now_mono().to_ns();
    data::Timestamp ts;
    if (cfg_.use_device_time) {
      // NOTE: despite the name, TIME_REFERENCE::IMAGE is a host-side
      // timestamp (Unix epoch ns) captured when the frame arrived in PC
      // memory — the ZED SDK does not expose a true sensor-exposure
      // timestamp.  It is still more consistent than now_mono() because
      // it is stamped inside the SDK's receive path, before our grab()
      // returns, so it excludes any scheduling jitter in our thread.
      uint64_t device_ts_ns =
        camera_->getTimestamp(sl::TIME_REFERENCE::IMAGE).getNanoseconds();
      ts.monotonic = data::Timespec::from_ns(device_ts_ns);
    } else {
      ts.monotonic = data::now_mono();
    }
    ts.realtime = data::now_real();

    // Inter-frame timing
    if (last_capture_mono_ != 0) {
      uint64_t dt = mono_now - last_capture_mono_;
      if_accum_ns_ += dt;
      if (dt > if_max_ns_) if_max_ns_ = dt;
      ++if_samples_;
    }
    last_capture_mono_ = mono_now;

    rec->ts = ts;
    rec->seq = seq_++;
    rec->id = cfg_.stream_id;

    try {
      emit(rec);
      ++stats_.produced;
    } catch (const std::exception& e) {
      ++stats_.dropped;
      std::cerr << "[ZedPushProducer] emit failed: " << e.what() << std::endl;
    } catch (...) {
      ++stats_.dropped;
      std::cerr << "[ZedPushProducer] emit failed with unknown exception"
                << std::endl;
    }

    // Periodic FPS health report
    if (cfg_.fps > 0 && stats_.produced >= next_health_report_frame_) {
      double avg_if_ms = 0.0;
      double max_if_ms = 0.0;
      if (if_samples_ > 0) {
        avg_if_ms = (if_accum_ns_ / 1e6) / static_cast<double>(if_samples_);
        max_if_ms = if_max_ns_ / 1e6;
      }
      double produced_fps = 0.0;
      if (if_samples_ > 0 && if_accum_ns_ > 0) {
        produced_fps =
          1e9 / (static_cast<double>(if_accum_ns_) / static_cast<double>(if_samples_));
      }
      if (produced_fps > 0 && (produced_fps + 0.5) < cfg_.fps) {
        std::cerr << "[ZedPushProducer] FPS health: produced=" << produced_fps
                  << " requested=" << cfg_.fps
                  << " avg_if_ms=" << avg_if_ms << " max_if_ms=" << max_if_ms
                  << std::endl;
      } else {
        std::cout << "[ZedPushProducer] FPS health: produced=" << produced_fps
                  << " requested=" << cfg_.fps
                  << " avg_if_ms=" << avg_if_ms << " max_if_ms=" << max_if_ms
                  << std::endl;
      }
      uint64_t interval = static_cast<uint64_t>(cfg_.fps) * 10;
      if (interval == 0) interval = 300;
      next_health_report_frame_ += interval;
    }
  }
}

// Register in PushProducerRegistry as "zed_camera"
REGISTER_PUSH_PRODUCER(ZedPushProducer, "zed_camera")

}  // namespace trossen::hw::camera
