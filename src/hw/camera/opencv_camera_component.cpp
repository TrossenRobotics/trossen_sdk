/**
 * @file opencv_camera_component.cpp
 * @brief Implementation of OpenCvCameraComponent
 */

#include <cmath>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "opencv2/opencv.hpp"

#include "trossen_sdk/hw/camera/opencv_camera_component.hpp"
#include "trossen_sdk/hw/discovery_registry.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/utils/camera_utils.hpp"

namespace trossen::hw::camera {

OpenCvCameraComponent::~OpenCvCameraComponent() {
  if (capture_ && capture_->isOpened()) {
    capture_->release();
  }
}

void OpenCvCameraComponent::configure(const nlohmann::json& config) {
  // Extract device index (required)
  if (!config.contains("device_index")) {
    throw std::runtime_error("OpenCvCameraComponent: 'device_index' is required in config");
  }
  device_index_ = config.at("device_index").get<int>();

  // Extract optional parameters
  if (config.contains("width")) {
    width_ = config.at("width").get<int>();
  }
  if (config.contains("height")) {
    height_ = config.at("height").get<int>();
  }
  if (config.contains("fps")) {
    fps_ = config.at("fps").get<int>();
  }

  // Parse backend (optional)
  if (config.contains("backend")) {
    std::string backend_str = config.at("backend").get<std::string>();
    if (backend_str == "v4l2" || backend_str == "V4L2") {
      backend_ = cv::CAP_V4L2;
    } else if (backend_str == "any" || backend_str == "auto") {
      backend_ = cv::CAP_ANY;
    } else {
      throw std::runtime_error(
        "OpenCvCameraComponent: Unknown backend: " + backend_str);
    }
  }

  // Create and open VideoCapture
  capture_ = std::make_shared<cv::VideoCapture>();

  if (!capture_->open(device_index_, backend_)) {
    throw std::runtime_error(
      "OpenCvCameraComponent: Failed to open camera device " +
      std::to_string(device_index_));
  }

  // Set camera parameters
  if (width_ > 0) {
    if (!capture_->set(cv::CAP_PROP_FRAME_WIDTH, width_)) {
      std::cerr << "Warning: Failed to set width to " << width_ << std::endl;
    }
  }
  if (height_ > 0) {
    if (!capture_->set(cv::CAP_PROP_FRAME_HEIGHT, height_)) {
      std::cerr << "Warning: Failed to set height to " << height_ << std::endl;
    }
  }
  if (fps_ > 0) {
    if (!capture_->set(cv::CAP_PROP_FPS, fps_)) {
      std::cerr << "Warning: Failed to set fps to " << fps_ << std::endl;
    }
  }

  // Read back actual values
  width_ = static_cast<int>(capture_->get(cv::CAP_PROP_FRAME_WIDTH));
  height_ = static_cast<int>(capture_->get(cv::CAP_PROP_FRAME_HEIGHT));
  fps_ = static_cast<int>(capture_->get(cv::CAP_PROP_FPS));

  // Get FOURCC for logging
  double fourcc = capture_->get(cv::CAP_PROP_FOURCC);
  char fourcc_chars[] = {
    static_cast<char>(static_cast<int>(fourcc) & 0xFF),
    static_cast<char>((static_cast<int>(fourcc) >> 8) & 0xFF),
    static_cast<char>((static_cast<int>(fourcc) >> 16) & 0xFF),
    static_cast<char>((static_cast<int>(fourcc) >> 24) & 0xFF),
    '\0'
  };

  std::cout << "Camera " << get_identifier() << " configured: "
            << width_ << "x" << height_ << " @ " << fps_
            << " FPS, FOURCC=" << fourcc_chars << std::endl;

  // Parse optional warmup_frames parameter
  if (config.contains("warmup_frames")) {
    warmup_frames_ = config.at("warmup_frames").get<int>();
  }

  // Perform warmup: discard initial frames to allow camera to stabilize
  if (warmup_frames_ > 0) {
    std::cout << "Warming up camera " << get_identifier() << ": discarding "
              << warmup_frames_ << " frames..." << std::endl;
    cv::Mat warmup_frame;
    int discarded = 0;
    for (int i = 0; i < warmup_frames_; ++i) {
      if (capture_->read(warmup_frame)) {
        ++discarded;
      } else {
        std::cerr << "Warning: Failed to read warmup frame " << i << std::endl;
      }
    }
    std::cout << "Warmup complete: discarded " << discarded << " frames" << std::endl;
  }
}

nlohmann::json OpenCvCameraComponent::get_info() const {
  nlohmann::json info = {
    {"type", "opencv_camera"},
    {"device_index", device_index_},
    {"width", width_},
    {"height", height_},
    {"fps", fps_}
  };

  // Add backend string
  if (backend_ == cv::CAP_V4L2) {
    info["backend"] = "v4l2";
  } else {
    info["backend"] = "auto";
  }

  // Add opened status
  info["is_opened"] = is_opened();

  return info;
}

bool OpenCvCameraComponent::is_opened() const {
  return capture_ && capture_->isOpened();
}

std::vector<DiscoveredHardware> OpenCvCameraComponent::find(
  const std::filesystem::path& output_dir)
{
  std::vector<DiscoveredHardware> results;

  for (int idx = 0; idx < 64; ++idx) {
    if (!std::filesystem::exists("/dev/video" + std::to_string(idx))) continue;

    // OpenCV prints a V4L2 error to stderr for every unopenable node; silence
    // it so the probe output stays clean.
    int devnull      = ::open("/dev/null", O_WRONLY);
    int saved_stderr = ::dup(2);
    ::dup2(devnull, 2);
    cv::VideoCapture cap(idx, cv::CAP_V4L2);
    ::dup2(saved_stderr, 2);
    ::close(saved_stderr);
    ::close(devnull);
    if (!cap.isOpened()) {
      cap.release();
      continue;
    }

    DiscoveredHardware info;
    info.type       = "opencv_camera";
    info.identifier = std::to_string(idx);

    std::filesystem::path preview_path = output_dir / ("opencv_" + info.identifier + ".jpg");
    info.details = {
      {"width",        static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH))},
      {"height",       static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT))},
      {"fps",          static_cast<int>(cap.get(cv::CAP_PROP_FPS))},
      {"preview_path", preview_path.string()},
    };

    // Read frames until we have one that is both valid (not black / blown out /
    // flat fill) and whose brightness has settled. is_valid_preview_frame()
    // gates the exit so a degenerate warmup frame is never committed.
    cv::Mat frame;
    cv::Mat best_frame;
    double last_brightness = -1.0;
    for (int w = 0; w < utils::kPreviewMaxWarmupFrames; ++w) {
      if (!cap.read(frame) || frame.empty()) continue;
      if (!utils::is_valid_preview_frame(frame)) continue;
      best_frame = frame.clone();
      double brightness = cv::mean(frame)[0];
      if (last_brightness > 0 &&
          std::abs(brightness - last_brightness) / last_brightness < 0.02) break;
      last_brightness = brightness;
    }

    info.ok = !best_frame.empty() && cv::imwrite(preview_path.string(), best_frame);
    if (!info.ok) {
      std::cerr << "[OpenCvCameraComponent::find] no valid frame captured for "
                << preview_path << "\n";
    }
    cap.release();

    results.push_back(info);
  }

  return results;
}

REGISTER_HARDWARE(OpenCvCameraComponent, "opencv_camera")
REGISTER_HARDWARE_DISCOVERY(OpenCvCameraComponent, "opencv_camera")

}  // namespace trossen::hw::camera
