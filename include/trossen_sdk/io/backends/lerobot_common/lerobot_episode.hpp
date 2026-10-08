/**
 * @file lerobot_episode.hpp
 * @brief Format-agnostic episode model and `features` schema shared by the LeRobot outputs.
 *
 * An AlignedEpisode is one episode reduced to a frame sequence: per-row action and
 * observation vectors plus the camera frame each row was matched to. Fields that hold source
 * data (dataset_info, CameraVideoStream) carry it as the recording supplied it.
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

#include "nlohmann/json.hpp"

#include "trossen_sdk/io/backend_utils.hpp"

namespace trossen::io::backends {

/// @brief One aligned dataset frame: synthetic timestamp + action/observation vectors.
struct AlignedFrame {
  /// @brief Frame timestamp in seconds (synthetic, generated at the dataset fps).
  float timestamp_s{0.0f};
  /// @brief Grid time this row was sampled at, as an MCAP log time in nanoseconds. The
  /// instant every stream in the row was matched against; kept for alignment diagnostics.
  uint64_t row_time_ns{0};
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
 * Produced when a recording stores cameras as `foxglove.CompressedVideo`. The payloads are
 * concatenated verbatim into an Annex B elementary stream, ready to remux (`ffmpeg -c copy`).
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
  /// @brief Source frame chosen for each dataset row: one entry per AlignedEpisode::frames,
  /// holding the 0-based arrival index of this camera's nearest-in-time frame.
  ///
  /// Consecutive rows name the same source frame when the camera runs slower than the row
  /// rate. Its size must equal AlignedEpisode::frames.size(); extract_camera_images() rejects
  /// a camera whose size differs.
  std::vector<size_t> row_source_index;
};

/// @brief Fully aligned, format-agnostic representation of one episode.
struct AlignedEpisode {
  /// @brief 0-based index of this episode in the output dataset.
  int episode_index{0};
  /// @brief Robot type written to the dataset's `robot_type`, from `dataset_info.robot_name`.
  std::string robot_name{DEFAULT_ROBOT_NAME};
  /// @brief Task prompt from the recording metadata's `task_description` field; may be empty.
  std::string task_name;
  /// @brief The recording's `dataset_info` blob (joint names, camera specs); may be empty.
  nlohmann::json dataset_info;

  /// @brief Stream ids whose joint positions form `action`, in vector order.
  std::vector<std::string> leader_streams;
  /// @brief Stream ids whose joint positions form `observation.state`, in vector order.
  std::vector<std::string> follower_streams;
  /// @brief Number of joints in every leader and follower stream.
  int joints_per_stream{0};
  /// @brief Whether the recording has a mobile base, whose block ends both vectors.
  bool has_mobile_base{false};
  /// @brief Dataset frame rate, in frames per second; rows are spaced 1 / fps seconds apart.
  float fps{DEFAULT_DATASET_FPS};

  /// @brief One entry per dataset row, misaligned rows already dropped.
  std::vector<AlignedFrame> frames;
  /// @brief Cameras present in the recording (names + LeRobot keys).
  std::vector<CameraInfo> cameras;
};

/// @brief Width of the mobile-base block appended to `action` and `observation.state`: the
/// linear and angular velocity, named by the recording's `mobile_base.velocity_names`.
inline constexpr int BASE_VELOCITY_WIDTH = 2;

/**
 * @brief Width of every AlignedFrame::action in @p ep.
 *
 * One positions block per leader stream, plus the base block for a mobile robot.
 *
 * @param ep Episode whose leader streams and base give the width.
 * @return Number of values in each action vector.
 */
int episode_action_dim(const AlignedEpisode& ep);

/**
 * @brief Width of every AlignedFrame::observation in @p ep.
 *
 * One positions block per follower stream, plus the base block for a mobile robot.
 *
 * @param ep Episode whose follower streams and base give the width.
 * @return Number of values in each observation vector.
 */
int episode_obs_dim(const AlignedEpisode& ep);

/**
 * @brief Build the LeRobot `features` schema for an episode.
 *
 * Not implemented yet: returns an empty object.
 *
 * @param ep Aligned episode (provides streams, cameras, dataset_info).
 * @return The `features` object for info.json.
 */
nlohmann::ordered_json build_features(const AlignedEpisode& ep);

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_EPISODE_HPP_
