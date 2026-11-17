/**
 * @file trossen_mcap_schemas.hpp
 * @brief TrossenMCAP topic naming conventions and schema helpers
 */

 #ifndef TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__TROSSEN_MCAP_SCHEMAS_HPP_
 #define TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__TROSSEN_MCAP_SCHEMAS_HPP_

#include <string>

namespace trossen::trossen_mcap_defs {

// Topic naming conventions (helpers only)
// Image topic: /cameras/<camera_name>/image
// Joint state topic: <robot_name>/joints/state (e.g., "/robots/default/joints/state")
// Optional per-camera metadata: /cameras/<camera_name>/meta

/// @brief `role` of a joint stream whose positions become the dataset action.
inline constexpr char kLeaderRole[] = "leader";

/// @brief `role` of a joint stream whose positions become the dataset observation.
inline constexpr char kFollowerRole[] = "follower";

/// @brief `storage` of a camera recorded as raw images.
inline constexpr char kRawStorage[] = "raw";

/// @brief `storage` of a color camera recorded as H.264 video.
inline constexpr char kH264Storage[] = "h264";

/// @brief `storage` of a depth camera recorded as lossless H.265 video.
inline constexpr char kH265Storage[] = "h265";

/// @brief Key inside the recording's metadata record holding the recording format version.
inline constexpr char kFormatVersionKey[] = "format_version";

/// @brief Recording format this SDK writes and reads. Raised when a key readers rely on is
/// added, removed or changes meaning; readers refuse any other version.
inline constexpr char kRecordingFormatVersion[] = "1";

/**
 * @brief Get topic name for a given camera's image stream
 *
 * @param camera_name Name of the camera
 * @return Topic name for the camera's image stream
 */
inline std::string image_topic(const std::string& camera_name) {
    return "/cameras/" + camera_name + "/image";
}

/**
 * @brief Get topic name for a given camera's metadata stream
 *
 * @param camera_name Name of the camera
 * @return Topic name for the camera's metadata stream
 */
inline std::string camera_meta_topic(const std::string& camera_name) {
    return "/cameras/" + camera_name + "/meta";
}

/**
 * @brief Get topic name for joint state stream of a given robot
 *
 * @param robot_name Name of the robot
 * @return Topic name for the robot's joint state stream
 */
inline std::string joint_state_topic(const std::string& robot_name) {
  return robot_name + "/joints/state";
}

/**
 * @brief Get topic name for 2D odometry stream
 *
 * @param stream_id Stream identifier (e.g., "base")
 * @return Topic name for the 2D odometry stream
 */
inline std::string odometry_2d_topic(const std::string& stream_id) {
  return stream_id + "/odom/state";
}

}  // namespace trossen::trossen_mcap_defs

#endif  // TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__TROSSEN_MCAP_SCHEMAS_HPP_
