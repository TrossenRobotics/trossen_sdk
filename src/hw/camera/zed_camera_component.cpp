/**
 * @file zed_camera_component.cpp
 * @brief Implementation of ZedCameraComponent
 *
 * Opens a StereoLabs ZED camera via sl::Camera::open() and exposes the
 * handle to ZedPushProducer.  Serial-number identification uses the unsigned
 * int overload of InputType::setFromSerialNumber() (standard for ZED cameras).
 */

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "trossen_sdk/hw/camera/zed_camera_component.hpp"
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
  close();
}

void ZedCameraComponent::close() {
  if (camera_ && camera_->isOpened()) {
    camera_->close();
    std::cout << "[ZedCameraComponent] Closed camera " << get_identifier()
              << " (S/N " << serial_number_ << ")\n";
  }
}

bool ZedCameraComponent::is_transient_open_error(sl::ERROR_CODE err) {
  switch (err) {
    // The camera is there but not yet ours: still allocated to a client the
    // driver has not reaped, mid-reboot, or enumerating. All clear on their own.
    case sl::ERROR_CODE::CANNOT_START_CAMERA_STREAM:
    case sl::ERROR_CODE::CAMERA_FAILED_TO_SETUP:
    case sl::ERROR_CODE::CAMERA_DETECTION_ISSUE:
    case sl::ERROR_CODE::CAMERA_NOT_DETECTED:
    case sl::ERROR_CODE::CAMERA_REBOOTING:
      return true;
    // Everything else — no GPU, bad resolution, SDK mismatch, DRIVER_FAILURE —
    // describes a rig that is misconfigured or a driver that needs restarting.
    // Waiting cannot fix any of them, and retrying only delays the real message.
    default:
      return false;
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
  open_retries_ = std::max(0, config.value("open_retries", kDefaultOpenRetries));
  open_retry_delay_s_ =
    std::max(0.0, config.value("open_retry_delay_s", kDefaultOpenRetryDelayS));

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

  // Open the camera, retrying while the failure is one that clears itself.
  //
  // The camera is dropped and rebuilt between attempts rather than reused: a
  // failed open() leaves the handle in an unspecified state, and the ZED SDK's
  // own guidance for a busy device is a fresh open, not a second call on the
  // same object.
  camera_ = std::make_shared<sl::Camera>();
  sl::ERROR_CODE err = camera_->open(init);
  for (int attempt = 1; attempt <= open_retries_ && err != sl::ERROR_CODE::SUCCESS;
       ++attempt)
  {
    if (!is_transient_open_error(err)) break;
    std::cerr << "[ZedCameraComponent] " << get_identifier() << " (S/N "
              << serial_number_ << ") open failed: " << sl::toString(err)
              << " — retrying in " << open_retry_delay_s_ << "s (" << attempt
              << "/" << open_retries_ << ")\n";
    std::this_thread::sleep_for(
      std::chrono::duration<double>(open_retry_delay_s_));
    camera_ = std::make_shared<sl::Camera>();
    err = camera_->open(init);
  }
  if (err != sl::ERROR_CODE::SUCCESS) {
    std::string hint;
    if (err == sl::ERROR_CODE::CANNOT_START_CAMERA_STREAM) {
      // Naming the likely cause matters here: the raw code reads like broken
      // hardware, and the operator's actual problem is almost always a previous
      // recorder that died holding this camera.
      hint =
        " (the camera is still held by another process — usually a recorder "
        "that crashed without releasing it)";
    } else if (err == sl::ERROR_CODE::DRIVER_FAILURE) {
      hint = " (GMSL driver failure — restart nvargus-daemon on the rig)";
    }
    throw std::runtime_error(
      "ZedCameraComponent: Failed to open ZED camera S/N " + serial_number_ +
      ": " + std::string(sl::toString(err).c_str()) + hint);
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

REGISTER_HARDWARE(ZedCameraComponent, "zed_camera")

}  // namespace trossen::hw::camera
