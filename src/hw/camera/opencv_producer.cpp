/**
 * @file opencv_producer.cpp
 * @brief Implementation of OpenCvCameraProducer that emits camera frames from a physical camera
 * device using OpenCV.
 */

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "trossen_sdk/hw/camera/opencv_camera_component.hpp"
#include "trossen_sdk/hw/camera/opencv_producer.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"

namespace trossen::hw::camera {

OpenCvCameraProducer::OpenCvCameraProducer(
  std::shared_ptr<HardwareComponent> hardware,
  const nlohmann::json& config)
{
  // Validate hardware type
  auto camera_component = std::dynamic_pointer_cast<OpenCvCameraComponent>(hardware);
  if (!camera_component) {
    throw std::runtime_error(
      "OpenCvCameraProducer requires OpenCvCameraComponent, got: " + hardware->get_type());
  }

  // Extract the underlying VideoCapture from the component
  cap_ = camera_component->get_hardware();
  if (!cap_) {
    throw std::runtime_error("OpenCvCameraComponent has null VideoCapture");
  }

  // Parse JSON config into internal Config struct
  cfg_.device_index = camera_component->get_device_index();
  cfg_.stream_id = config.value("stream_id", "camera" + std::to_string(cfg_.device_index));
  cfg_.encoding = config.value("encoding", "bgr8");
  cfg_.width = config.value("width", 0);
  cfg_.height = config.value("height", 0);
  cfg_.fps = config.value("fps", 0);
  cfg_.use_device_time = config.value("use_device_time", true);
  cfg_.enforce_requested_fps = config.value("enforce_requested_fps", true);

  // Parse preferred_fourcc if provided
  if (config.contains("preferred_fourcc") && config["preferred_fourcc"].is_array()) {
    cfg_.preferred_fourcc.clear();
    for (const auto& fourcc_str : config["preferred_fourcc"]) {
      if (fourcc_str.is_string()) {
        std::string s = fourcc_str.get<std::string>();
        if (s.length() == 4) {
          int32_t code = cv::VideoWriter::fourcc(s[0], s[1], s[2], s[3]);
          cfg_.preferred_fourcc.push_back(code);
        }
      }
    }
  }

  // Populate metadata
  metadata_.type = "opencv_camera";
  metadata_.id = cfg_.stream_id;
  metadata_.name = cfg_.stream_id;
  metadata_.description = "Produces camera frames from a physical camera device using OpenCV";
  metadata_.width = cfg_.width;
  metadata_.height = cfg_.height;
  metadata_.fps = cfg_.fps;
  metadata_.codec = "av1";
  metadata_.pix_fmt = "yuv420p";
  metadata_.channels = (cfg_.encoding == "bgr8" || cfg_.encoding == "rgb8") ? 3 : 1;
  metadata_.has_audio = false;
}

void OpenCvCameraProducer::poll(
  const std::function<void(std::shared_ptr<data::RecordBase>)>& emit)
{
  // TODO(lukeschmitt-tr): silently fails if cannot open
  if (!cap_ || !cap_->isOpened()) {
    return;
  }

  auto img = std::make_shared<data::ImageRecord>();
  if (!cap_->read(img->image)) {
    ++stats_.dropped;
    return;
  }

  data::Timestamp ts;
  // TODO(lukeschmitt-tr): use device timestamp if available and cfg_.use_device_time
  uint64_t mono_now = data::now_mono().to_ns();
  ts.monotonic = data::now_mono();
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
REGISTER_PRODUCER(OpenCvCameraProducer, "opencv_camera")

}  // namespace trossen::hw::camera
