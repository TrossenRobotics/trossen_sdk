/**
 * @file lerobot_episode.hpp
 * @brief Format-agnostic episode model and `features` schema shared by the LeRobot outputs.
 *
 * An AlignedEpisode is one episode reduced to a frame sequence: per-row action and
 * observation vectors plus the camera frame each row was matched to. Its types name no
 * source format, so it is equally the product of reading a recording offline and of
 * buffering samples during a live capture. Fields that hold source data (dataset_info,
 * CameraVideoStream) carry it as the recording supplied it.
 *
 * build_features() turns one into the LeRobot `features` schema, which is identical
 * between v2.1 and v3.0.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_EPISODE_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_EPISODE_HPP_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trossen_sdk/io/backend_utils.hpp"

namespace trossen::io::backends {

/**
 * @brief Selects which decoded signals are written into the dataset vectors.
 *
 * Every signal a recording carries is decoded regardless of these flags; they only choose
 * what reaches `action` and `observation.state`. The defaults produce positions-only joint
 * blocks and the planar base velocity pair.
 */
struct DatasetSignalOptions {
  /// @brief Append each stream's joint velocities (`.vel`) to observation.state.
  bool joint_velocity{false};
  /// @brief Append each stream's joint efforts (`.eff`) to observation.state.
  bool joint_effort{false};
  /// @brief Append the base lateral velocity (Twist2D.linear_y) to the base block.
  bool base_lateral_velocity{false};
  /// @brief Append the base pose (Pose2D x, y, theta) to the base block.
  bool base_pose{false};
};

/// @brief One aligned dataset frame: synthetic timestamp + action/observation vectors.
struct AlignedFrame {
  /// @brief Frame timestamp in seconds (synthetic, generated at the dataset fps).
  float timestamp_s{0.0f};
  /// @brief Reference-stream log time this row was sampled at, in nanoseconds. The instant
  /// every stream in the row was matched against; kept for alignment diagnostics.
  uint64_t reference_timestamp_ns{0};
  /// @brief Leader joints (+ base velocities for mobile robots), the LeRobot `action`.
  /// Its length is episode_action_dim().
  std::vector<double> action;
  /// @brief Follower joints (+ base velocities), the LeRobot `observation.state`.
  /// Its length is episode_obs_dim().
  std::vector<double> observation;
};

/**
 * @brief One camera's already-compressed video stream, taken straight from the recording.
 *
 * Produced when a recording stores cameras as `foxglove.CompressedVideo` rather
 * than `foxglove.RawImage`. The payloads are concatenated verbatim into an Annex
 * B elementary stream, so the converter can remux (`ffmpeg -c copy`) instead of
 * decoding and re-encoding. The frames were already encoded once at capture time.
 */
struct CameraVideoStream {
  /// @brief Annex B elementary stream on disk (`.h264` / `.hevc`).
  std::filesystem::path annexb_path;
  /// @brief Bitstream format as recorded: "h264" or "h265".
  std::string format;
  /// @brief Number of video messages, i.e. frames (one message per frame).
  size_t frame_count{0};
};

/// @brief A camera present in the recording.
struct CameraInfo {
  /// @brief Bare camera name, e.g. "cam_high".
  std::string name;
  /// @brief LeRobot video key, e.g. "observation.images.cam_high".
  std::string obs_key;
  /// @brief Number of frames extracted (filled by extract_camera_images(), declared in #330
  /// and implemented in #307).
  size_t frame_count{0};
  /// @brief Source frame chosen for each dataset row: one entry per AlignedEpisode::frames,
  /// holding the 0-based arrival index of this camera's nearest-in-time frame.
  ///
  /// Cameras free-run on their own clock, so their frames neither start with nor keep pace
  /// with the joint streams; pairing the two by position drifts. Each row therefore records
  /// which source frame actually belongs to it, and extract_camera_images() writes exactly
  /// those frames, in row order. Consecutive rows may name the same source frame when the
  /// camera runs slower than the reference stream.
  ///
  /// Its size must equal AlignedEpisode::frames.size(); extract_camera_images() rejects a
  /// camera whose size differs.
  ///
  /// TODO(shantanuparab-tr): improve this synchronization logic. Nearest-timestamp matching
  /// repeats a source frame whenever a camera runs slower than the row rate, so a row's image
  /// can be up to the tolerance old.
  std::vector<size_t> row_source_index;
};

/// @brief Fully aligned, format-agnostic representation of one episode.
struct AlignedEpisode {
  int episode_index{0};
  std::string robot_name{DEFAULT_ROBOT_NAME};
  /// @brief Task prompt from the recording's `dataset_info.task`; may be empty.
  std::string task_name;
  /// @brief The recording's `dataset_info` blob (joint names, camera specs); may be empty.
  nlohmann::json dataset_info;

  std::vector<std::string> leader_streams;
  std::vector<std::string> follower_streams;
  int joints_per_stream{0};
  bool has_mobile_base{false};
  float fps{DEFAULT_DATASET_FPS};

  /// @brief One entry per dataset row, misaligned rows already dropped.
  std::vector<AlignedFrame> frames;
  /// @brief Cameras present in the recording (names + LeRobot keys).
  std::vector<CameraInfo> cameras;
};

/**
 * @brief Width of the mobile-base block appended to `action` and `observation.state`.
 *
 * The linear and angular velocity pair, then the lateral velocity and the 3-value pose
 * when @p signals enables them.
 */
int base_block_width(const DatasetSignalOptions& signals);

/**
 * @brief Width of every AlignedFrame::action in @p ep.
 *
 * One positions block per leader stream, plus the base block for a mobile robot.
 * @param signals Must match the value passed to load_aligned_episode() (#329).
 */
int episode_action_dim(const AlignedEpisode& ep, const DatasetSignalOptions& signals = {});

/**
 * @brief Width of every AlignedFrame::observation in @p ep.
 *
 * Per follower stream, the positions block plus a velocity and an effort block when
 * @p signals enables them; then the base block for a mobile robot.
 * @param signals Must match the value passed to load_aligned_episode() (#329).
 */
int episode_obs_dim(const AlignedEpisode& ep, const DatasetSignalOptions& signals = {});

/**
 * @brief Build the LeRobot `features` schema for an episode.
 *
 * Declared here and implemented in #309; until then it returns an empty object.
 *
 * Produces the observation.state / action / observation.images.<cam> entries with
 * joint names (from dataset_info when present) and camera specs, plus the standard
 * scalar metadata features. The schema is identical between LeRobot v2.1 and v3.0.
 *
 * @param ep Aligned episode (provides streams, cameras, dataset_info).
 * @param native_schema Use lerobot_trossen naming (`<side>_joint_<i>.pos` joints, `cam_*`
 *   camera keys) and describe depth cameras as 12-bit depth video rather than color.
 * @param signals Must match the value passed to load_aligned_episode() (#329); the column
 *   names produced here describe the vectors it built.
 * @return The `features` object for info.json.
 */
nlohmann::ordered_json build_features(
  const AlignedEpisode& ep,
  bool native_schema = false,
  const DatasetSignalOptions& signals = {});

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_EPISODE_HPP_
