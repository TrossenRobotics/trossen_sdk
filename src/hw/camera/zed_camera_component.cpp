/**
 * @file zed_camera_component.cpp
 * @brief Implementation of ZedCameraComponent
 *
 * Opens a StereoLabs ZED camera via sl::Camera::open() and exposes the
 * handle to ZedPushProducer.  Serial-number identification uses the unsigned
 * int overload of InputType::setFromSerialNumber() (standard for ZED cameras).
 */

#include <iostream>
#include <stdexcept>
#include <string>

#include "opencv2/opencv.hpp"

#include "trossen_sdk/hw/camera/zed_camera_component.hpp"
#include "trossen_sdk/hw/discovery_registry.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"

namespace trossen::hw::camera {

// ─────────────────────────────────────────────────────────────
// Helpers: string → enum with deprecation warnings
// ─────────────────────────────────────────────────────────────

sl::DEPTH_MODE ZedCameraComponent::parse_depth_mode(const std::string& mode_str) {
  // SDK 5.x marks PERFORMANCE, QUALITY, and ULTRA as deprecated.
  // Emit a clear warning so users migrate to the NEURAL family.
  if (mode_str == "PERFORMANCE") {
    std::cerr << "[ZedCameraComponent] WARNING: DEPTH_MODE::PERFORMANCE is "
                 "deprecated in ZED SDK 5.x. Prefer NEURAL_LIGHT.\n";
    return sl::DEPTH_MODE::PERFORMANCE;
  }
  if (mode_str == "QUALITY") {
    std::cerr << "[ZedCameraComponent] WARNING: DEPTH_MODE::QUALITY is "
                 "deprecated in ZED SDK 5.x. Prefer NEURAL.\n";
    return sl::DEPTH_MODE::QUALITY;
  }
  if (mode_str == "ULTRA") {
    std::cerr << "[ZedCameraComponent] WARNING: DEPTH_MODE::ULTRA is "
                 "deprecated in ZED SDK 5.x. Prefer NEURAL_PLUS.\n";
    return sl::DEPTH_MODE::ULTRA;
  }
  if (mode_str == "NEURAL_LIGHT") return sl::DEPTH_MODE::NEURAL_LIGHT;
  if (mode_str == "NEURAL")       return sl::DEPTH_MODE::NEURAL;
  if (mode_str == "NEURAL_PLUS")  return sl::DEPTH_MODE::NEURAL_PLUS;
  if (mode_str == "NONE")         return sl::DEPTH_MODE::NONE;

  std::cerr << "[ZedCameraComponent] Unknown depth_mode '" << mode_str
            << "', falling back to NEURAL.\n";
  return sl::DEPTH_MODE::NEURAL;
}

sl::RESOLUTION ZedCameraComponent::parse_resolution(const std::string& res_str) {
  if (res_str == "HD2K")   return sl::RESOLUTION::HD2K;
  if (res_str == "HD1080") return sl::RESOLUTION::HD1080;
  if (res_str == "HD1200") return sl::RESOLUTION::HD1200;
  if (res_str == "HD720")  return sl::RESOLUTION::HD720;
  if (res_str == "SVGA")   return sl::RESOLUTION::SVGA;
  if (res_str == "VGA")    return sl::RESOLUTION::VGA;
  if (res_str == "AUTO")   return sl::RESOLUTION::AUTO;

  std::cerr << "[ZedCameraComponent] Unknown resolution '" << res_str
            << "', falling back to AUTO.\n";
  return sl::RESOLUTION::AUTO;
}

// ─────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────

ZedCameraComponent::~ZedCameraComponent() {
  if (camera_ && camera_->isOpened()) {
    camera_->close();
    std::cout << "[ZedCameraComponent] Closed camera " << get_identifier()
              << " (S/N " << serial_number_ << ")\n";
  }
}

void ZedCameraComponent::configure(const nlohmann::json& config) {
  // Serial number (required) — accept both string and numeric JSON values
  if (!config.contains("serial_number")) {
    throw std::runtime_error(
      "ZedCameraComponent: 'serial_number' is required in config");
  }
  if (config["serial_number"].is_number()) {
    serial_number_ = std::to_string(config["serial_number"].get<uint64_t>());
  } else {
    serial_number_ = config["serial_number"].get<std::string>();
  }

  // Optional resolution / fps / depth settings
  std::string resolution_str = config.value("resolution", "HD720");
  int requested_fps = config.value("fps", 0);
  use_depth_ = config.value("use_depth", false);
  depth_mode_str_ = config.value("depth_mode", std::string("NONE"));

  // Build InitParameters
  sl::InitParameters init;
  init.camera_resolution = parse_resolution(resolution_str);
  init.camera_fps = requested_fps;
  init.depth_mode = use_depth_
    ? parse_depth_mode(depth_mode_str_)
    : sl::DEPTH_MODE::NONE;
  init.coordinate_units = sl::UNIT::MILLIMETER;

  // Identify camera by serial number
  unsigned int sn_uint = 0;
  try {
    sn_uint = static_cast<unsigned int>(std::stoul(serial_number_));
  } catch (...) {
    throw std::runtime_error(
      "ZedCameraComponent: serial_number must be a numeric string, got: " +
      serial_number_);
  }
  init.input.setFromSerialNumber(sn_uint);

  // Open camera
  camera_ = std::make_shared<sl::Camera>();
  sl::ERROR_CODE err = camera_->open(init);
  if (err != sl::ERROR_CODE::SUCCESS) {
    throw std::runtime_error(
      "ZedCameraComponent: Failed to open ZED camera S/N " + serial_number_ +
      ": " + std::string(sl::toString(err).c_str()));
  }

  // Read back negotiated resolution
  auto cam_info = camera_->getCameraInformation();
  width_ = static_cast<int>(cam_info.camera_configuration.resolution.width);
  height_ = static_cast<int>(cam_info.camera_configuration.resolution.height);
  fps_ = static_cast<int>(cam_info.camera_configuration.fps);

  std::cout << "[ZedCameraComponent] " << get_identifier() << " opened: "
            << width_ << "x" << height_ << " @ " << fps_ << " FPS"
            << " (depth=" << (use_depth_ ? depth_mode_str_ : "off") << ")\n";
}

nlohmann::json ZedCameraComponent::get_info() const {
  nlohmann::json info = {
    {"type", "zed_camera"},
    {"serial_number", serial_number_},
    {"width", width_},
    {"height", height_},
    {"fps", fps_},
    {"use_depth", use_depth_},
    {"depth_mode", depth_mode_str_},
    {"is_opened", is_opened()}
  };
  return info;
}

bool ZedCameraComponent::is_opened() const {
  return camera_ && camera_->isOpened();
}

std::vector<DiscoveredHardware> ZedCameraComponent::find(
  const std::filesystem::path& /*output_dir*/)
{
  std::cout << "[ZedCameraComponent::find] not yet implemented\n";
  return {};
}

REGISTER_HARDWARE(ZedCameraComponent, "zed_camera")
REGISTER_HARDWARE_DISCOVERY(ZedCameraComponent, "zed_camera")

}  // namespace trossen::hw::camera
