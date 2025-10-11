/**
 * @file camera_types.hpp
 * @brief Shared camera capability and depth format enums & helpers for multi-stream (color + depth) support.
 */

#ifndef TROSSEN_SDK__HW__CAMERA__CAMERA_TYPES_HPP
#define TROSSEN_SDK__HW__CAMERA__CAMERA_TYPES_HPP

#include <stdexcept>
#include <string>

namespace trossen::hw::camera {

/**
 * @brief Camera capability profile.
 */
enum class CameraCapability {
  /// @brief Color stream only (e.g. standard RGB camera)
  ColorOnly,

  /// @brief Synchronized color + depth streams (e.g. RGB-D camera)
  ColorAndDepth
};

/**
 * @brief Depth pixel representation format.
 */
enum class DepthFormat {
  /// @brief 16-bit depth scaled by depth_scale_m - used by RealSense cameras
  DEPTH16,

  /// @brief 32-bit depth reported in meters - used by StereoLabs ZED cameras.
  FLOAT32
};

/**
 * @brief Map DepthFormat to RawImage.encoding string (foxglove RawImage reuse).
 *
 * @param fmt Depth format
 * @return Corresponding encoding string
 */
inline std::string depth_encoding_string(DepthFormat fmt) {
  switch (fmt) {
    case DepthFormat::DEPTH16: return "16UC1";
    case DepthFormat::FLOAT32: return "32FC1";
  }
  return "UNKNOWN";
}

/**
 * @brief Whether this depth format requires an external scale factor (depth_scale_m) to convert to
 * meters.
 */
inline bool depth_uses_scale(DepthFormat fmt) { return fmt == DepthFormat::DEPTH16; }

/**
 * @brief Convenience to decide if depth is enabled based on capability flag.
 *
 * @param cap Camera capability
 * @return true if depth is enabled, false otherwise
 */
inline bool depth_enabled(CameraCapability cap) {
  return cap == CameraCapability::ColorAndDepth;
}

} // namespace trossen::hw::camera

#endif // TROSSEN_SDK__HW__CAMERA__CAMERA_TYPES_HPP
