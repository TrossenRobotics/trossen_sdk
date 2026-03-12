/**
 * @file realsense_push_producer.cpp
 * @brief Implementation of RealsensePushProducer
 */

#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "opencv2/imgproc.hpp"

#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/camera/realsense_camera_component.hpp"
#include "trossen_sdk/hw/camera/realsense_push_producer.hpp"
#include "trossen_sdk/runtime/push_producer_registry.hpp"

namespace trossen::hw::camera {

RealsensePushProducer::RealsensePushProducer(
  std::shared_ptr<::trossen::hw::HardwareComponent> hardware,
  const nlohmann::json& config)
{
  auto cam = std::dynamic_pointer_cast<RealsenseCameraComponent>(hardware);
  if (!cam) {
    throw std::runtime_error(
      "RealsensePushProducer requires RealsenseCameraComponent, got: " +
      hardware->get_type());
  }

  pipeline_ = cam->get_hardware();
  if (!pipeline_) {
    throw std::runtime_error(
      "RealsensePushProducer: RealsenseCameraComponent has null pipeline");
  }

  use_depth_ = cam->get_use_depth();
  depth_scale_ = cam->get_depth_scale();

  // Create aligner once here (reused per frame) if depth is enabled
  if (use_depth_ && cam->get_align_depth_to_color()) {
    aligner_ = std::make_unique<rs2::align>(RS2_STREAM_COLOR);
  }

  // Parse producer config
  cfg_.stream_id = config.value("stream_id", cam->get_identifier());
  cfg_.color_encoding = config.value("encoding", "bgr8");
  cfg_.use_device_time = config.value("use_device_time", true);
  cfg_.timeout_ms = config.value("timeout_ms", 3000);
  if (cfg_.timeout_ms <= 0) {
    cfg_.timeout_ms = 3000;
  }
  cfg_.fps = config.value("fps", 30);

  // Populate metadata
  metadata_.type = "realsense_camera";
  metadata_.id = cfg_.stream_id;
  metadata_.name = cfg_.stream_id;
  metadata_.description =
    "Unified push producer for RealSense camera (color + optional depth)";
  // Width/height/fps from camera component's actual negotiated pipeline profile values
  metadata_.width = cam->get_width();
  metadata_.height = cam->get_height();
  metadata_.fps = cam->get_fps();
  metadata_.codec = "av1";
  metadata_.pix_fmt = "yuv420p";
  metadata_.channels = 3;
  metadata_.has_audio = false;
  metadata_.has_depth = use_depth_;

  std::cout << "[RealsensePushProducer] Constructed for stream '"
            << cfg_.stream_id << "' (depth=" << (use_depth_ ? "on" : "off") << ")"
            << std::endl;
}

RealsensePushProducer::~RealsensePushProducer() {
  stop();
}

bool RealsensePushProducer::start(
  const std::function<void(std::shared_ptr<data::RecordBase>)>& emit)
{
  if (running_.load()) {
    std::cerr << "[RealsensePushProducer] Already running." << std::endl;
    return false;
  }

  running_.store(true);
  push_thread_ = std::thread([this, emit]() { push_loop(emit); });

  std::cout << "[RealsensePushProducer] Started push thread for stream '"
            << cfg_.stream_id << "' (depth=" << (use_depth_ ? "on" : "off") << ")"
            << std::endl;
  return true;
}

void RealsensePushProducer::stop() {
  if (!running_.load()) {
    return;
  }

  running_.store(false);

  if (push_thread_.joinable()) {
    push_thread_.join();
  }

  std::cout << "[RealsensePushProducer] Stopped push thread for stream '"
            << cfg_.stream_id << "'. produced=" << stats_.produced
            << " dropped=" << stats_.dropped << std::endl;
}

void RealsensePushProducer::push_loop(
  const std::function<void(std::shared_ptr<data::RecordBase>)>& emit)
{
  while (running_.load(std::memory_order_relaxed)) {
    rs2::frameset frames;
    try {
      frames = pipeline_->wait_for_frames(cfg_.timeout_ms);
    } catch (const rs2::error& e) {
      std::cerr << "[RealsensePushProducer] wait_for_frames failed: "
                << e.what() << std::endl;
      ++stats_.dropped;
      continue;
    }

    // Optional depth alignment (aligner_ is non-null only when both use_depth_ and
    // align_depth_to_color are set)
    if (use_depth_ && aligner_) {
      frames = aligner_->process(frames);
    }

    auto color_frame = frames.get_color_frame();
    if (!color_frame) {
      ++stats_.dropped;
      continue;
    }

    const int w = color_frame.get_width();
    const int h = color_frame.get_height();

    // Build color cv::Mat (camera delivers RGB8; convert to requested encoding)
    cv::Mat color_raw(h, w, CV_8UC3,
                      const_cast<void*>(color_frame.get_data()),
                      cv::Mat::AUTO_STEP);
    cv::Mat color_out;
    if (cfg_.color_encoding == "bgr8") {
      cv::cvtColor(color_raw, color_out, cv::COLOR_RGB2BGR);
    } else if (cfg_.color_encoding == "rgb8") {
      // Clone required: color_raw wraps the rs2::frame buffer which is recycled
      // on the next wait_for_frames() call
      color_out = color_raw.clone();
    } else {
      throw std::runtime_error(
        "[RealsensePushProducer] Unsupported color encoding: " + cfg_.color_encoding);
    }

    auto rec = std::make_shared<data::ImageRecord>();
    rec->image = std::move(color_out);
    rec->width = static_cast<uint32_t>(w);
    rec->height = static_cast<uint32_t>(h);
    rec->channels = 3;
    rec->encoding = cfg_.color_encoding;

    // Optional depth frame
    if (use_depth_) {
      auto depth_frame = frames.get_depth_frame();
      if (depth_frame) {
        const int depth_w = depth_frame.get_width();
        const int depth_h = depth_frame.get_height();
        cv::Mat depth_raw(depth_h, depth_w, CV_16UC1,
                          const_cast<void*>(depth_frame.get_data()),
                          cv::Mat::AUTO_STEP);
        rec->depth_image = depth_raw.clone();
        rec->depth_scale = depth_scale_;
      }
      // If depth frame is null for this frameset, emit color-only record
    }

    // Timestamp
    uint64_t mono_now = data::now_mono().to_ns();
    data::Timestamp ts;
    if (cfg_.use_device_time) {
      double device_ts_ms = color_frame.get_timestamp();
      uint64_t device_ts_ns = static_cast<uint64_t>(device_ts_ms * 1'000'000.0);
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
      std::cerr << "[RealsensePushProducer] emit failed: " << e.what() << std::endl;
    } catch (...) {
      ++stats_.dropped;
      std::cerr << "[RealsensePushProducer] emit failed with unknown exception" << std::endl;
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
        std::cerr << "[RealsensePushProducer] FPS health: produced=" << produced_fps
                  << " requested=" << cfg_.fps
                  << " avg_if_ms=" << avg_if_ms << " max_if_ms=" << max_if_ms << std::endl;
      } else {
        std::cout << "[RealsensePushProducer] FPS health: produced=" << produced_fps
                  << " requested=" << cfg_.fps
                  << " avg_if_ms=" << avg_if_ms << " max_if_ms=" << max_if_ms << std::endl;
      }
      uint64_t interval = static_cast<uint64_t>(cfg_.fps) * 10;
      if (interval == 0) interval = 300;
      next_health_report_frame_ += interval;
    }
  }
}

// Register in PushProducerRegistry as "realsense_camera"
REGISTER_PUSH_PRODUCER(RealsensePushProducer, "realsense_camera")

}  // namespace trossen::hw::camera
