/**
 * @file realsense_producer.cpp
 * @brief Implementation of RealsenseCameraProducer that emits camera frames from a physical camera
 * device using RealSense SDK.
 */

#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "trossen_sdk/hw/camera/realsense_camera_component.hpp"
#include "trossen_sdk/hw/camera/realsense_producer.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"

namespace trossen::hw::camera {

RealsenseCameraProducer::RealsenseCameraProducer(
  std::shared_ptr<HardwareComponent> hardware,
  const nlohmann::json& config)
{
  // Validate hardware type
  auto camera_component = std::dynamic_pointer_cast<RealsenseCameraComponent>(hardware);
  if (!camera_component) {
    throw std::runtime_error(
      "RealsenseCameraProducer requires RealsenseCameraComponent, got: " + hardware->get_type());
  }

  // Extract the underlying RealsenseFrameCache from the component
  frame_cache_ = camera_component->get_hardware();
  if (!frame_cache_) {
    throw std::runtime_error("RealsenseCameraComponent has null RealsenseFrameCache");
  }

  // Parse JSON config into internal Config struct
  cfg_.serial_number = camera_component->get_serial_number();
  cfg_.stream_id = config.value("stream_id", "realsense_camera_" + cfg_.serial_number);
  cfg_.encoding = config.value("encoding", "bgr8");
  cfg_.width = config.value("width", 0);
  cfg_.height = config.value("height", 0);
  cfg_.fps = config.value("fps", 0);
  cfg_.use_device_time = config.value("use_device_time", true);
  cfg_.warmup_seconds = config.value("warmup_seconds", 0.0);
  cfg_.enforce_requested_fps = config.value("enforce_requested_fps", true);

  // Populate metadata
  metadata_.type = "realsense_camera";
  metadata_.id = cfg_.stream_id;
  metadata_.name = cfg_.stream_id;
  metadata_.description = "Produces camera frames from a physical camera device using Realsense";
  metadata_.width = cfg_.width;
  metadata_.height = cfg_.height;
  metadata_.fps = cfg_.fps;
  metadata_.codec = "av1";
  metadata_.pix_fmt = "yuv420p";
  metadata_.channels = (cfg_.encoding == "bgr8" || cfg_.encoding == "rgb8") ? 3 : 1;
  metadata_.has_audio = false;
}

void RealsenseCameraProducer::poll(
  const std::function<void(std::shared_ptr<data::RecordBase>)>& emit)
{
  // TODO(lukeschmitt-tr): silently fails if cannot open
  if (!frame_cache_) {
    return;
  }

  auto img = std::make_shared<data::ImageRecord>();

  rs2::frameset frames;
  try {
    frames = frame_cache_->get_frames(3000);
  } catch (const rs2::error& e) {
    std::cout << "[WARN] RealSense poll failed: " << e.what() << std::endl;
    ++stats_.dropped;
    return;
  }

  auto color_frame = frames.get_color_frame();
  if (!color_frame) {
    std::cout << "[WARN] RealSense poll failed: " << std::endl;
    ++stats_.dropped;
    return;
  }

  // Convert rs2::frame to cv::Mat
  const void* data_ptr = static_cast<const void*>(color_frame.get_data());
  cv::Mat image(cv::Size(cfg_.width, cfg_.height), CV_8UC3,
                const_cast<void*>(data_ptr), cv::Mat::AUTO_STEP);
  // BGR format
  cv::cvtColor(image, image, cv::COLOR_RGB2BGR);
  img->image = std::move(image);

  data::Timestamp ts;
  // TODO(lukeschmitt-tr): use device timestamp if available and cfg_.use_device_time

  uint64_t mono_now = data::now_mono().to_ns();

  if (cfg_.use_device_time) {
    uint64_t device_ts = color_frame.get_timestamp();
    ts.monotonic = data::Timespec::from_ns(device_ts);
  } else {
    ts.monotonic = data::now_mono();
  }
  ts.realtime = data::now_real();

  // Inter-frame timing instrumentation
  if (last_capture_mono_ != 0) {
    uint64_t dt = mono_now - last_capture_mono_;
    if_accum_ns_ += dt;
    if (dt > if_max_ns_) if_max_ns_ = dt;
    ++if_samples_;
  }
  last_capture_mono_ = mono_now;

  img->ts = ts;
  img->seq = seq_++;
  img->id = cfg_.stream_id;
  img->width = static_cast<uint32_t>(img->image.cols);
  img->height = static_cast<uint32_t>(img->image.rows);
  img->encoding = cfg_.encoding;
  img->channels = static_cast<uint32_t>(img->image.channels());

  emit(img);
  ++stats_.produced;

  // Periodically report FPS health

  // TODO(lukeschmitt-tr): make this all configurable via config
  if (cfg_.fps > 0 && stats_.produced >= next_health_report_frame_) {
    double avg_if_ms = 0.0;
    double max_if_ms = 0.0;
    if (if_samples_ > 0) {
      avg_if_ms = (if_accum_ns_ / 1e6) / static_cast<double>(if_samples_);
      max_if_ms = if_max_ns_ / 1e6;
    }
    double produced_fps = 0.0;
    if (if_samples_ > 0 && if_accum_ns_ > 0) {
      produced_fps = 1e9 / (static_cast<double>(if_accum_ns_) / static_cast<double>(if_samples_));
    }
    if (produced_fps > 0 && (produced_fps + 0.5) < cfg_.fps) {
      std::cerr << "Camera FPS health: produced_fps=" << produced_fps << " requested=" << cfg_.fps
                << " avg_if_ms=" << avg_if_ms << " max_if_ms=" << max_if_ms << std::endl;
    } else {
      std::cout << "Camera FPS health: produced_fps=" << produced_fps << " requested=" << cfg_.fps
                << " avg_if_ms=" << avg_if_ms << " max_if_ms=" << max_if_ms << std::endl;
    }
    // schedule next report ~10 seconds later
    uint64_t interval = static_cast<uint64_t>(cfg_.fps) * 10;
    if (interval == 0) interval = 300;
    next_health_report_frame_ += interval;
  }
}

// Register with ProducerRegistry
REGISTER_PRODUCER(RealsenseCameraProducer, "realsense_camera")

}  // namespace trossen::hw::camera
