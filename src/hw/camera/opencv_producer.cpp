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
#include "trossen_sdk/runtime/push_producer_registry.hpp"

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
  // The size and rate the device opened at, which can differ from the request (0 asks for
  // the device default), so the recording describes the frames it actually holds.
  cfg_.width = camera_component->get_width();
  cfg_.height = camera_component->get_height();
  cfg_.fps = camera_component->get_fps();
  if (cfg_.fps <= 0) {
    throw std::runtime_error(
      "OpenCvCameraProducer: camera " + std::to_string(cfg_.device_index) +
      " reports no frame rate; set its fps in the config");
  }
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

OpenCvCameraProducer::~OpenCvCameraProducer() {
  stop();
}

bool OpenCvCameraProducer::start(
  const std::function<void(std::shared_ptr<data::RecordBase>)>& emit)
{
  if (running_.load()) {
    std::cerr << "[OpenCvCameraProducer] Already running." << std::endl;
    return false;
  }
  if (!cap_->isOpened()) {
    std::cerr << "[OpenCvCameraProducer] camera '" << cfg_.stream_id
              << "' is not open; check that /dev/video" << cfg_.device_index
              << " exists and no other program uses it" << std::endl;
    return false;
  }

  fps_health_.restart();
  running_.store(true);
  push_thread_ = std::thread([this, emit]() { push_loop(emit); });
  return true;
}

void OpenCvCameraProducer::stop() {
  if (!running_.load()) {
    return;
  }
  running_.store(false);
  if (push_thread_.joinable()) {
    push_thread_.join();
  }
}

void OpenCvCameraProducer::push_loop(
  const std::function<void(std::shared_ptr<data::RecordBase>)>& emit)
{
  while (running_.load(std::memory_order_relaxed)) {
    auto img = std::make_shared<data::ImageRecord>();
    // Blocks until the camera delivers the next frame.
    if (!cap_->read(img->image)) {
      ++stats_.dropped;
      continue;
    }

    data::Timestamp ts;
    // TODO(lukeschmitt-tr): use device timestamp if available and cfg_.use_device_time
    const uint64_t mono_now = data::now_mono().to_ns();
    ts.monotonic = data::now_mono();
    ts.realtime = data::now_real();

    fps_health_.on_frame(mono_now);

    img->ts = ts;
    img->seq = seq_++;
    img->id = cfg_.stream_id;
    img->width = static_cast<uint32_t>(img->image.cols);
    img->height = static_cast<uint32_t>(img->image.rows);
    img->encoding = cfg_.encoding;
    img->channels = static_cast<uint32_t>(img->image.channels());

    try {
      emit(img);
      ++stats_.produced;
    } catch (const std::exception& e) {
      ++stats_.dropped;
      std::cerr << "[OpenCvCameraProducer] emit failed: " << e.what() << std::endl;
    } catch (...) {
      ++stats_.dropped;
      std::cerr << "[OpenCvCameraProducer] emit failed with unknown exception" << std::endl;
    }
  }
}

// Register in PushProducerRegistry as "opencv_camera"
REGISTER_PUSH_PRODUCER(OpenCvCameraProducer, "opencv_camera")

}  // namespace trossen::hw::camera
