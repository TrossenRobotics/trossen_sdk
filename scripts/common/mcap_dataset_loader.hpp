/**
 * @file mcap_dataset_loader.hpp
 * @brief Format-agnostic MCAP reading + frame alignment shared by the LeRobot converters.
 *
 * Both `trossen_mcap_to_lerobot_v2` and `trossen_mcap_to_lerobot_v3` read the same
 * TrossenMCAP recordings and align the same async joint/camera streams into a single
 * per-episode frame sequence. Only the on-disk *output* layout differs between the two.
 * This header exposes the shared input side:
 *
 *   - load_aligned_episode(): MCAP decode + leader/follower detection + nearest-timestamp
 *     alignment → an in-memory AlignedEpisode (one row per dataset frame).
 *   - extract_camera_images(): decode camera frames to a caller-chosen directory layout.
 *   - build_features(): the LeRobot `features` schema (identical between v2.1 and v3.0).
 *
 * The MCAP implementation (MCAP_IMPLEMENTATION) is compiled into this library's
 * translation unit only; executables link against it and must NOT define the macro.
 */

#ifndef TROSSEN_SDK__SCRIPTS__COMMON__MCAP_DATASET_LOADER_HPP_
#define TROSSEN_SDK__SCRIPTS__COMMON__MCAP_DATASET_LOADER_HPP_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "mcap/reader.hpp"
#include "nlohmann/json.hpp"

namespace trossen::convert {

/// @brief One decoded joint-state sample from a single stream.
struct JointStateMessage {
  uint64_t timestamp_ns{0};
  std::vector<double> positions;
  std::vector<double> velocities;
  std::string stream_id;
};

/// @brief One decoded mobile-base odometry sample (linear + angular velocity).
struct Odometry2DMessage {
  uint64_t timestamp_ns{0};
  double vel_x{0.0};
  double vel_theta{0.0};
};

/// @brief MCAP channel-id → semantic-stream maps, populated during detection.
struct McapChannelMap {
  /// @brief Joint-state channels → stream id (e.g. "leader", "follower_left").
  std::map<mcap::ChannelId, std::string> joint_channels;
  /// @brief Image channels → camera name (e.g. "cam_high").
  std::map<mcap::ChannelId, std::string> camera_channels;
  /// @brief Mobile-base odometry channel (valid only when has_slate_base is true).
  mcap::ChannelId slate_base_channel_id{0};
  /// @brief Whether a mobile-base odometry channel was found.
  bool has_slate_base{false};
};

/// @brief One aligned dataset frame: synthetic timestamp + action/observation vectors.
struct AlignedFrame {
  /// @brief Frame timestamp in seconds (synthetic, generated at the dataset fps).
  float timestamp_s{0.0f};
  /// @brief Leader joints (+ base velocities for mobile robots), the LeRobot `action`.
  std::vector<double> action;
  /// @brief Follower joints (+ base velocities), the LeRobot `observation.state`.
  std::vector<double> observation;
};

/// @brief A camera present in the recording.
struct CameraInfo {
  /// @brief Bare camera name, e.g. "cam_high".
  std::string name;
  /// @brief LeRobot video key, e.g. "observation.images.cam_high".
  std::string obs_key;
  /// @brief Number of frames extracted (filled by extract_camera_images()).
  size_t frame_count{0};
};

/// @brief Fully aligned, format-agnostic representation of one episode.
struct AlignedEpisode {
  int episode_index{0};
  std::string robot_name{"trossen_solo_ai"};
  /// @brief Task prompt read from the MCAP's embedded `dataset_info.task`.
  /// Empty when the recording carries no task — the converter then falls back
  /// to its configured `task_name`. Distinct per episode, which is what lets a
  /// single recording session yield a multi-task LeRobot dataset.
  std::string task_name{};
  /// @brief The MCAP-embedded `dataset_info` blob (joint names, camera specs); may be empty.
  nlohmann::json mcap_dataset_info;

  std::vector<std::string> leader_streams;
  std::vector<std::string> follower_streams;
  int joints_per_stream{0};
  bool has_slate_base{false};
  int action_dim{0};
  int obs_dim{0};
  float fps{30.0f};

  /// @brief One entry per dataset row, misaligned rows already dropped.
  std::vector<AlignedFrame> frames;
  /// @brief Cameras present in the recording (names + LeRobot keys).
  std::vector<CameraInfo> cameras;
};

/**
 * @brief Read an MCAP file and align its joint/camera streams into a frame sequence.
 *
 * Decodes the embedded dataset_info metadata, auto-detects leader/follower joint
 * streams by topic name (falling back to single-robot mode), parses all joint and
 * odometry messages, and produces one AlignedFrame per dataset row via
 * nearest-timestamp matching (50 ms tolerance). Rows where any stream has no sample
 * within tolerance are dropped. Camera frames are NOT decoded here — call
 * extract_camera_images() for that.
 *
 * @param mcap_file Path to the input MCAP file.
 * @param episode_index Zero-based episode index to stamp on the episode.
 * @param out Output episode (overwritten on success).
 * @param channels Output channel maps (reused by extract_camera_images()).
 * @return true on success; false on a fatal error (message logged to stderr).
 */
bool load_aligned_episode(
  const std::string& mcap_file,
  int episode_index,
  AlignedEpisode& out,
  McapChannelMap& channels,
  bool native_schema = false);

/**
 * @brief Decode camera frames from an MCAP file into caller-chosen directories.
 *
 * Re-opens the MCAP file and writes each camera's frames as `image_%06d.jpg`
 * (JPEG quality 95) into the directory returned by `dir_for(camera_name)`. The
 * callback is invoked once per camera and is responsible for creating the dir.
 *
 * @param mcap_file Path to the input MCAP file.
 * @param channels Channel maps from load_aligned_episode().
 * @param dir_for Maps a camera name to the directory its frames are written to.
 * @param out_counts Output per-camera frame counts.
 * @return true on success; false on a fatal error (message logged to stderr).
 */
bool extract_camera_images(
  const std::string& mcap_file,
  const McapChannelMap& channels,
  const std::function<std::filesystem::path(const std::string& camera_name)>& dir_for,
  std::map<std::string, size_t>& out_counts,
  bool native_schema = false);

/**
 * @brief Build the LeRobot `features` schema for an episode.
 *
 * Produces the observation.state / action / observation.images.<cam> entries with
 * joint names (from dataset_info when present) and camera specs, plus the standard
 * scalar metadata features. The schema is identical between LeRobot v2.1 and v3.0.
 *
 * @param ep Aligned episode (provides streams, dims, dataset_info).
 * @param channels Channel maps (provides the camera list).
 * @return The `features` object for info.json.
 */
nlohmann::ordered_json build_features(
  const AlignedEpisode& ep, const McapChannelMap& channels, bool native_schema = false);

}  // namespace trossen::convert

#endif  // TROSSEN_SDK__SCRIPTS__COMMON__MCAP_DATASET_LOADER_HPP_
