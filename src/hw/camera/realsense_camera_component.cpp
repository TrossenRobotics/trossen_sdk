/**
 * @file realsense_camera_component.cpp
 * @brief Implementation of RealsenseCameraComponent
 */

#include <iostream>
#include <stdexcept>
#include <string>

#include "trossen_sdk/hw/camera/realsense_camera_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"

namespace trossen::hw::camera {

RealsenseCameraComponent::~RealsenseCameraComponent() {
  // TODO(shantanuparab-tr): Properly stop and release Realsense resources
  std::cout << "RealsenseCameraComponent: Destructor called for "
            << get_identifier() << std::endl;
}

void RealsenseCameraComponent::configure(const nlohmann::json& config) {
  // Extract device index (required)
  if (!config.contains("serial_number")) {
    throw std::runtime_error("RealsenseCameraComponent: 'serial_number' is required in config");
  }
  serial_number = config.at("serial_number").get<std::string>();

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
  if (config.contains("use_depth")) {
    use_depth_ = config.at("use_depth").get<bool>();
  }
  if (config.contains("align_depth_to_color")) {
    align_depth_to_color_ = config.at("align_depth_to_color").get<bool>();
  }
  if (config.contains("force_hardware_reset")) {
    force_hardware_reset_ = config.at("force_hardware_reset").get<bool>();
  }

  // TODO(shantanuparab-tr): Improve this logic to avoid iterating all connected devices
  // Force hardware reset if requested
  if (force_hardware_reset_) {
    std::cout << "RealsenseCameraComponent: Forcing hardware reset for camera "
              << serial_number << std::endl;
    // Find the devices connected
    rs2::context ctx;
    auto devices = ctx.query_devices();
    // Reset the device with matching serial number
    for (auto&& dev : devices) {
      if (dev.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER) == serial_number) {
        dev.hardware_reset();
        std::cout << "RealsenseCameraComponent: Hardware reset completed for camera "
                  << serial_number << std::endl;
        break;
      }
    }
  }

  // Create Realsense pipeline
  rs2::pipeline realsense_pipeline;
  auto camera_ = std::make_shared<rs2::pipeline>(realsense_pipeline);

  // Create Realsense config
  rs2::config cam_cfg;

  // Enable the device using the serial number
  cam_cfg.enable_device(serial_number);

  // Enable color stream by default
  cam_cfg.enable_stream(RS2_STREAM_COLOR, width_, height_, RS2_FORMAT_RGB8, fps_);

  // Enable depth stream if requested
  if (use_depth_) {
    cam_cfg.enable_stream(RS2_STREAM_DEPTH, width_, height_, RS2_FORMAT_Z16, fps_);
  }

  // Start the camera pipeline
  try {
    profile = camera_->start(cam_cfg);
  } catch (const rs2::error& e) {
    throw std::runtime_error(
      "RealsenseCameraComponent: Failed to start camera with serial number " +
      serial_number + ": " + std::string(e.what()));
  }

  // Set pipeline_ only after successful start
  pipeline_ = camera_;

  // Store depth scale if depth stream is enabled
  if (use_depth_) {
    try {
      auto depth_sensor = profile.get_device().first<rs2::depth_sensor>();
      depth_scale_ = depth_sensor.get_depth_scale();
    } catch (const rs2::error& e) {
      std::cerr << "RealsenseCameraComponent: Could not read depth scale: "
                << e.what() << std::endl;
      depth_scale_ = 0.001f;  // fallback: 1mm per Z16 unit
    }
  }

  // Read back actual values
  auto color_stream = profile.get_stream(RS2_STREAM_COLOR);
  auto color_profile = color_stream.as<rs2::video_stream_profile>();
  width_ = color_profile.width();
  height_ = color_profile.height();
  fps_ = static_cast<int>(color_profile.fps());

  // If depth stream is enabled, confirm its parameters
  if (use_depth_) {
    auto depth_stream = profile.get_stream(RS2_STREAM_DEPTH);
    auto depth_profile = depth_stream.as<rs2::video_stream_profile>();
    // Just to confirm depth stream is valid
    (void)depth_profile.width();
    (void)depth_profile.height();
    (void)depth_profile.fps();
  }

  std::cout << "Camera " << get_identifier() << " configured: "
            << width_ << "x" << height_ << " @ " << fps_
            << " FPS" << std::endl;
}

nlohmann::json RealsenseCameraComponent::get_info() const {
  nlohmann::json info = {
    {"type", "realsense_camera"},
    {"serial_number", serial_number},
    {"width", width_},
    {"height", height_},
    {"fps", fps_},
    {"use_depth", use_depth_},
    {"force_hardware_reset", force_hardware_reset_}
  };

  // Add opened status
  info["is_opened"] = is_opened();

  return info;
}

bool RealsenseCameraComponent::is_opened() const {
  if (!pipeline_) {
    return false;
  }
  try {
    pipeline_->get_active_profile();
    return true;
  } catch (const rs2::error&) {
    return false;
  }
}

REGISTER_HARDWARE(RealsenseCameraComponent, "realsense_camera")

}  // namespace trossen::hw::camera
