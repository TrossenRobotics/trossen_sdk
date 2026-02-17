/**
 * @file camera_config.hpp
 * @brief Configuration for camera components
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__ROBOTS__COMPONENTS__CAMERA_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__ROBOTS__COMPONENTS__CAMERA_CONFIG_HPP_

#include <string>
#include <nlohmann/json.hpp>

namespace trossen::configuration {

/**
 * @brief Configuration for a single camera
 *
 * Supports OpenCV cameras and RealSense depth cameras
 */
struct CameraConfig {
  /// Camera type ("opencv", "realsense", "mock")
  std::string type{"opencv"};

  /// Unique device identifier: serial number for RealSense,
  //  device index as string for OpenCV (e.g., "0", "1")
  std::string unique_device_id{"0"};

  /// Image width in pixels
  int width{640};

  /// Image height in pixels
  int height{480};

  /// Frame rate in frames per second
  int fps{30};

  /// Image encoding format (e.g., "rgb8", "bgr8", "jpeg")
  std::string encoding{"rgb8"};

  /// Whether to use device timestamps (true) or system timestamps (false)
  bool use_device_time{false};

  /// Enable depth stream for RealSense cameras
  bool enable_depth{false};

  /// Optional identifier for this camera
  std::string id{""};

  /**
   * @brief Create CameraConfig from JSON
   * @param j JSON object containing camera configuration
   * @return Populated CameraConfig with defaults for missing fields
   */
  static CameraConfig from_json(const nlohmann::json& j) {
    CameraConfig c;

    if (j.contains("type")) j.at("type").get_to(c.type);

    // Handle unique_device_id with backward compatibility
    if (j.contains("unique_device_id")) {
      j.at("unique_device_id").get_to(c.unique_device_id);
    } else if (j.contains("serial_number")) {
      // Backward compatibility: map serial_number to unique_device_id
      j.at("serial_number").get_to(c.unique_device_id);
    } else if (j.contains("device_index")) {
      // Backward compatibility: map device_index to unique_device_id
      c.unique_device_id = std::to_string(j.at("device_index").get<int>());
    }

    if (j.contains("width")) j.at("width").get_to(c.width);
    if (j.contains("height")) j.at("height").get_to(c.height);
    if (j.contains("fps")) j.at("fps").get_to(c.fps);
    if (j.contains("encoding")) j.at("encoding").get_to(c.encoding);
    if (j.contains("use_device_time")) j.at("use_device_time").get_to(c.use_device_time);
    if (j.contains("enable_depth")) j.at("enable_depth").get_to(c.enable_depth);
    if (j.contains("id")) j.at("id").get_to(c.id);

    return c;
  }

  /**
   * @brief Convert CameraConfig to JSON
   * @return JSON representation of this configuration
   */
  nlohmann::json to_json() const {
    nlohmann::json j;
    j["type"] = type;
    j["unique_device_id"] = unique_device_id;
    j["width"] = width;
    j["height"] = height;
    j["fps"] = fps;
    j["encoding"] = encoding;
    j["use_device_time"] = use_device_time;
    j["enable_depth"] = enable_depth;
    if (!id.empty()) j["id"] = id;
    return j;
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__ROBOTS__COMPONENTS__CAMERA_CONFIG_HPP_
