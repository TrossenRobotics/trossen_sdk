/**
 * @file trossen_mcap_schemas.hpp
 * @brief TrossenMCAP topic naming conventions and schema helpers
 */

 #ifndef TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__TROSSEN_MCAP_SCHEMAS_HPP_
 #define TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__TROSSEN_MCAP_SCHEMAS_HPP_

#include <string>
#include <string_view>

namespace trossen::trossen_mcap_defs {

// Topic naming conventions (helpers only)
// Image topic: /cameras/<camera_name>/image
// Joint state topic: <robot_name>/joints/state (e.g., "/robots/default/joints/state")
// Optional per-camera metadata: /cameras/<camera_name>/meta

/// @brief Prefix every camera topic sits under.
inline constexpr char kCameraTopicPrefix[] = "/cameras/";

/// @brief Suffix marking a camera topic as the image stream.
inline constexpr char kImageTopicSuffix[] = "/image";

/// @brief Suffix marking a camera topic as the per-camera metadata stream.
inline constexpr char kCameraMetaTopicSuffix[] = "/meta";

/// @brief Suffix a stream id carries when the topic holds its joint states.
inline constexpr char kJointStateTopicSuffix[] = "/joints/state";

/// @brief Suffix a stream id carries when the topic holds its planar odometry.
inline constexpr char kOdometry2DTopicSuffix[] = "/odom/state";

/// @brief Role a leader arm's stream id is named with: `leader` or `leader_<side>`.
inline constexpr char kLeaderStreamToken[] = "leader";

/// @brief Role a follower arm's stream id is named with: `follower` or `follower_<side>`.
inline constexpr char kFollowerStreamToken[] = "follower";

/// @brief Name of the file-level metadata record written once per recording.
inline constexpr char kRecordingMetadataName[] = "trossen_sdk_recording";

/// @brief Key inside that record holding the dataset_info JSON blob.
inline constexpr char kDatasetInfoKey[] = "dataset_info";

/**
 * @brief Check whether a stream id is named with a role: exactly `role`, or `role_<side>`.
 *
 * @param stream_id Stream id to check, e.g. `follower_left`.
 * @param role Role name, kLeaderStreamToken or kFollowerStreamToken.
 * @return True if @p stream_id is @p role alone or @p role followed by `_` and a side name.
 */
inline bool stream_id_has_role(std::string_view stream_id, std::string_view role) {
  if (!stream_id.starts_with(role)) return false;
  const std::string_view rest = stream_id.substr(role.size());
  return rest.empty() || (rest.size() > 1 && rest.front() == '_');
}

/**
 * @brief Check whether a stream id names a leader arm (`leader` or `leader_<side>`).
 *
 * @param stream_id Stream id to check.
 * @return True if the stream belongs to a leader arm.
 */
inline bool is_leader_stream_id(std::string_view stream_id) {
  return stream_id_has_role(stream_id, kLeaderStreamToken);
}

/**
 * @brief Check whether a stream id names a follower arm (`follower` or `follower_<side>`).
 *
 * @param stream_id Stream id to check.
 * @return True if the stream belongs to a follower arm.
 */
inline bool is_follower_stream_id(std::string_view stream_id) {
  return stream_id_has_role(stream_id, kFollowerStreamToken);
}

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
 * @brief Get topic name for a given camera's metadata stream
 *
 * @param camera_name Name of the camera
 * @return Topic name for the camera's metadata stream
 */
inline std::string camera_meta_topic(const std::string& camera_name) {
    return kCameraTopicPrefix + camera_name + kCameraMetaTopicSuffix;
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
