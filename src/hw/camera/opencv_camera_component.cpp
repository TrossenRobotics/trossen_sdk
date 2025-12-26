/**
 * @file opencv_camera_component.cpp
 * @brief Implementation of OpenCvCameraComponent
 */

 #include <iostream>
#include <stdexcept>
#include <string>

#include "trossen_sdk/hw/camera/opencv_camera_component.hpp"

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

}  // namespace trossen::hw::camera
