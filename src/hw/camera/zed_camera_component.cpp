/**
 * @file zed_camera_component.cpp
 * @brief Implementation of ZedCameraComponent
 */

#include <iostream>
#include <stdexcept>
#include <string>

#include "trossen_sdk/hw/camera/zed_camera_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"

namespace trossen::hw::camera {

// Helper to parse resolution string
static sl::RESOLUTION parse_resolution(const std::string& res_str) {
  if (res_str == "VGA") return sl::RESOLUTION::VGA;
  if (res_str == "SVGA") return sl::RESOLUTION::SVGA;
  if (res_str == "HD720") return sl::RESOLUTION::HD720;
  if (res_str == "HD1080") return sl::RESOLUTION::HD1080;
  if (res_str == "HD2K") return sl::RESOLUTION::HD2K;

  std::cerr << "Unknown resolution: " << res_str << ", defaulting to SVGA\n";
  return sl::RESOLUTION::SVGA;
}

// Helper to parse depth mode string
static sl::DEPTH_MODE parse_depth_mode(const std::string& mode_str) {
  if (mode_str == "NONE") return sl::DEPTH_MODE::NONE;
  if (mode_str == "PERFORMANCE") return sl::DEPTH_MODE::PERFORMANCE;
  if (mode_str == "QUALITY") return sl::DEPTH_MODE::QUALITY;
  if (mode_str == "ULTRA") return sl::DEPTH_MODE::ULTRA;
  if (mode_str == "NEURAL") return sl::DEPTH_MODE::NEURAL;

  std::cerr << "Unknown depth mode: " << mode_str << ", defaulting to NONE\n";
  return sl::DEPTH_MODE::NONE;
}

ZedCameraComponent::~ZedCameraComponent() {
  if (camera_ && camera_->isOpened()) {
    std::cout << "ZedCameraComponent: Destructor called for " << get_identifier() << std::endl;
    camera_->close();
  }
}

void ZedCameraComponent::configure(const nlohmann::json& config) {
  // Extract device index
  if (config.contains("serial_number")) {
    serial_number_ = config.at("serial_number").get<int>();
  }

  // Extract optional parameters
  if (config.contains("resolution")) {
    std::string res_str = config.at("resolution").get<std::string>();
    resolution_ = parse_resolution(res_str);
  }

  if (config.contains("fps")) {
    fps_ = config.at("fps").get<int>();
  }

  if (config.contains("use_depth")) {
    use_depth_ = config.at("use_depth").get<bool>();
  }

  if (config.contains("depth_mode")) {
    std::string mode_str = config.at("depth_mode").get<std::string>();
    depth_mode_ = parse_depth_mode(mode_str);
  }

  // Create camera instance
  camera_ = std::make_shared<sl::Camera>();

  // Configure initialization parameters
  sl::InitParameters init_params;
  init_params.camera_resolution = resolution_;
  init_params.camera_fps = fps_;
  init_params.depth_mode = depth_mode_;
  init_params.coordinate_units = sl::UNIT::METER;
  init_params.sdk_verbose = false;

  // Set serial number if specified (0 = use any camera)
  if (serial_number_ > 0) {
    init_params.input.setFromSerialNumber(serial_number_);
  }

  // Open camera
  sl::ERROR_CODE err = camera_->open(init_params);
  if (err != sl::ERROR_CODE::SUCCESS) {
    throw std::runtime_error(
      "ZedCameraComponent: Failed to open camera " + get_identifier() +
      ": " + std::string(sl::toString(err)));
  }

  // Get actual camera information
  sl::CameraInformation cam_info = camera_->getCameraInformation();
  sl::CalibrationParameters calib = cam_info.camera_configuration.calibration_parameters;

  // Store actual serial number if we didn't specify one
  if (serial_number_ == 0) {
    serial_number_ = cam_info.serial_number;
  }

  // Get actual resolution
  width_ = calib.left_cam.image_size.width;
  height_ = calib.left_cam.image_size.height;

  // Create frame cache (1 consumer for color only, 2 for color+depth)
  size_t num_consumers = use_depth_ ? 2 : 1;
  frame_cache_ = std::make_shared<ZedFrameCache>(camera_, num_consumers);

  std::cout << "ZedCameraComponent: Camera " << get_identifier()
            << " opened successfully\n";
  std::cout << "  Serial Number: " << serial_number_ << "\n";
  std::cout << "  Model: " << sl::toString(cam_info.camera_model) << "\n";
  std::cout << "  Resolution: " << width_ << "x" << height_ << " @ " << fps_ << " FPS\n";
  std::cout << "  Depth: " << (use_depth_ ? "Enabled" : "Disabled") << "\n";
}

nlohmann::json ZedCameraComponent::get_info() const {
  nlohmann::json info = {
    {"type", "zed_camera"},
    {"serial_number", serial_number_},
    {"width", width_},
    {"height", height_},
    {"fps", fps_},
    {"use_depth", use_depth_},
    {"depth_mode", sl::toString(depth_mode_)}
  };

  info["is_opened"] = is_opened();

  return info;
}

bool ZedCameraComponent::is_opened() const {
  return camera_ && camera_->isOpened();
}

REGISTER_HARDWARE(ZedCameraComponent, "zed_camera")

}  // namespace trossen::hw::camera
