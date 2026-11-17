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

/// @brief Prefix every camera topic sits under.
inline constexpr char kCameraTopicPrefix[] = "/cameras/";

/// @brief Suffix marking a camera topic as the image stream.
inline constexpr char kImageTopicSuffix[] = "/image";

/// @brief Suffix a stream id carries when the topic holds its joint states.
inline constexpr char kJointStateTopicSuffix[] = "/joints/state";

/// @brief Suffix a stream id carries when the topic holds its planar odometry.
inline constexpr char kOdometry2DTopicSuffix[] = "/odom/state";

/// @brief Name of the file-level metadata record written once per recording.
inline constexpr char kRecordingMetadataName[] = "trossen_sdk_recording";

/// @brief Key inside that record holding the dataset_info JSON blob.
inline constexpr char kDatasetInfoKey[] = "dataset_info";

/// @brief Key inside that record holding the recording start, in nanoseconds since the Unix epoch.
inline constexpr char kRecordingStartTimeKey[] = "recording_start_time";

/// @brief Key inside that record holding the episode's task prompt, when one was set.
inline constexpr char kTaskDescriptionKey[] = "task_description";

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
    return kCameraTopicPrefix + camera_name + kImageTopicSuffix;
}

/**
 * @brief Get topic name for joint state stream of a given robot
 *
 * @param robot_name Name of the robot
 * @return Topic name for the robot's joint state stream
 */
inline std::string joint_state_topic(const std::string& robot_name) {
  return robot_name + kJointStateTopicSuffix;
}

/**
 * @brief Get topic name for 2D odometry stream
 *
 * @param stream_id Stream identifier (e.g., "base")
 * @return Topic name for the 2D odometry stream
 */
inline std::string odometry_2d_topic(const std::string& stream_id) {
  return stream_id + kOdometry2DTopicSuffix;
}

}  // namespace trossen::trossen_mcap_defs

#endif  // TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__TROSSEN_MCAP_SCHEMAS_HPP_
