/**
 * @file camera_config.hpp
 * @brief Configuration for a camera hardware component
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__CAMERA_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__CAMERA_CONFIG_HPP_

#include <string>

#include "nlohmann/json.hpp"

namespace trossen::configuration {

/**
 * @brief Configuration for a single camera (RealsenseCameraComponent or OpenCvCameraComponent)
 *
 * The @p type field selects which hardware component is created via HardwareRegistry.
 * Use @c "realsense_camera" for Intel RealSense cameras and @c "opencv_camera" for
 * any V4L2 / USB webcam accessible through OpenCV.
 *
 * JSON format (RealSense):
 * @code
 * {
 *   "id": "camera_0",
 *   "type": "realsense_camera",
 *   "serial_number": "128422271347",
 *   "width": 640,
 *   "height": 480,
 *   "fps": 30,
 *   "use_depth": false,
 *   "force_hardware_reset": false
 * }
 * @endcode
 *
 * JSON format (OpenCV):
 * @code
 * {
 *   "id": "camera_0",
 *   "type": "opencv_camera",
 *   "device_index": 0,
 *   "width": 640,
 *   "height": 480,
 *   "fps": 30,
 *   "backend": "v4l2"
 * }
 * @endcode
 */
struct CameraConfig {
  /// @brief Hardware registry key - "realsense_camera" or "opencv_camera"
  std::string type{"realsense_camera"};

  /// @brief Logical camera identifier used as stream_id (e.g. "camera_0", "wrist_cam")
  std::string id{"camera_0"};

  /// @brief Camera serial number (RealSense only)
  std::string serial_number{""};

  /// @brief OpenCV device index (OpenCV only)
  int device_index{0};

  /// @brief OpenCV capture backend - "v4l2" or "any" (OpenCV only)
  std::string backend{""};

  /// @brief Capture width in pixels
  int width{640};

  /// @brief Capture height in pixels
  int height{480};

  /// @brief Capture frame rate
  int fps{30};

  /// @brief Enable depth stream alongside color (RealSense only)
  bool use_depth{false};

  /// @brief Force hardware reset on open (RealSense only)
  bool force_hardware_reset{false};

  static CameraConfig from_json(const nlohmann::json& j) {
    CameraConfig c;
    if (j.contains("type")) j.at("type").get_to(c.type);
    if (j.contains("id")) j.at("id").get_to(c.id);
    if (j.contains("serial_number")) j.at("serial_number").get_to(c.serial_number);
    if (j.contains("device_index")) j.at("device_index").get_to(c.device_index);
    if (j.contains("backend")) j.at("backend").get_to(c.backend);
    if (j.contains("width")) j.at("width").get_to(c.width);
    if (j.contains("height")) j.at("height").get_to(c.height);
    if (j.contains("fps")) j.at("fps").get_to(c.fps);
    if (j.contains("use_depth")) j.at("use_depth").get_to(c.use_depth);
    if (j.contains("force_hardware_reset"))
      j.at("force_hardware_reset").get_to(c.force_hardware_reset);
    return c;
  }

  /// @brief Produce JSON suitable for HardwareComponent::configure()
  nlohmann::json to_json() const {
    nlohmann::json j{
      {"serial_number", serial_number},
      {"device_index", device_index},
      {"width", width},
      {"height", height},
      {"fps", fps},
      {"use_depth", use_depth},
      {"force_hardware_reset", force_hardware_reset}
    };
    if (!backend.empty()) {
      j["backend"] = backend;
    }
    return j;
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__CAMERA_CONFIG_HPP_
