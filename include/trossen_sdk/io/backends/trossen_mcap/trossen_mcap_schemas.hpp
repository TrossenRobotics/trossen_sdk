/**
 * @file trossen_mcap_schemas.hpp
 * @brief TrossenMCAP topic naming conventions and schema helpers
 */

 #ifndef TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__TROSSEN_MCAP_SCHEMAS_HPP_
 #define TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__TROSSEN_MCAP_SCHEMAS_HPP_

#include <cstdio>
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

/// @brief Key inside that record holding the recording start, in nanoseconds since epoch.
inline constexpr char kRecordingStartTimeKey[] = "recording_start_time";

/// @brief Key inside that record holding the episode's task prompt, when one was set.
inline constexpr char kTaskDescriptionKey[] = "task_description";

/// @brief Key inside that record holding how camera frames are stored: "raw" or "video".
inline constexpr char kImageEncodingKey[] = "image_encoding";

/// @brief Key inside that record holding the SDK version that wrote it, "vMAJOR.MINOR.PATCH".
inline constexpr char kToolVersionKey[] = "tool_version";

/// @brief Oldest SDK version whose recordings this SDK reads.
inline constexpr char kMinToolVersion[] = "v0.1.1";

/**
 * @brief Check whether a recording's `tool_version` is one this SDK reads.
 *
 * @param version The recording's `tool_version`, e.g. "v0.1.1".
 * @return True if it parses as "vMAJOR.MINOR.PATCH" and is at least kMinToolVersion.
 */
inline bool tool_version_supported(std::string_view version) {
  // Parses "vMAJOR.MINOR.PATCH" into three numbers; false on anything else. %n stores how many
  // characters were read, so trailing text such as "-rc1" fails.
  const auto parse = [](std::string_view text, unsigned (&parts)[3]) {
    const std::string s(text);
    int read = -1;
    return std::sscanf(s.c_str(), "v%u.%u.%u%n", &parts[0], &parts[1], &parts[2], &read) == 3 &&
           read == static_cast<int>(s.size());
  };
  unsigned found[3];
  unsigned minimum[3];
  if (!parse(version, found) || !parse(kMinToolVersion, minimum)) return false;
  for (int i = 0; i < 3; ++i) {
    if (found[i] != minimum[i]) return found[i] > minimum[i];
  }
  return true;
}

/**
 * @brief Why a recording's `tool_version` is refused, for an error after the file name.
 *
 * @param version The recording's `tool_version`; empty when it has none.
 */
inline std::string unsupported_tool_version_message(std::string_view version) {
  return "was recorded with SDK " +
         (version.empty() ? std::string("of unknown version") : std::string(version)) +
         "; this SDK reads recordings from " + kMinToolVersion + " on";
}

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
