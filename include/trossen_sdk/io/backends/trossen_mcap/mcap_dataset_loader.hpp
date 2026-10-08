/**
 * @file mcap_dataset_loader.hpp
 * @brief Reading a TrossenMCAP recording into an AlignedEpisode.
 *
 * The reader half of the TrossenMCAP backend: it decodes a finished recording, detects
 * leader/follower joint streams, and nearest-timestamp aligns them into one frame
 * sequence. Camera frames are matched per row and extracted on demand, either decoded to
 * images or copied out as an already-compressed video stream.
 *
 * The output is the format-agnostic AlignedEpisode, so a consumer is free to write a
 * LeRobot dataset, drive a visualizer, or do anything else with a decoded recording.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__MCAP_DATASET_LOADER_HPP_
#define TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__MCAP_DATASET_LOADER_HPP_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>

#include "mcap/reader.hpp"

#include "trossen_sdk/io/backends/lerobot_common/lerobot_episode.hpp"

namespace trossen::io::backends {

/// @brief MCAP channel-id → semantic-stream maps, populated during detection.
struct McapChannelMap {
  /// @brief Joint-state channels → stream id (e.g. "leader", "follower_left").
  std::map<mcap::ChannelId, std::string> joint_channels;
  /// @brief Image channels → camera name (e.g. "cam_high").
  std::map<mcap::ChannelId, std::string> camera_channels;
  /// @brief Mobile-base odometry channel (valid only when has_mobile_base is true).
  mcap::ChannelId mobile_base_channel_id{0};
  /// @brief Whether a mobile-base odometry channel was found.
  bool has_mobile_base{false};
};

/// @brief Timing parameters for nearest-timestamp alignment.
struct AlignmentOptions {
  /// @brief Rate the output rows are generated at, in frames per second.
  double fps{30.0};
  /// @brief Largest gap, in nanoseconds, allowed between a row and the sample it matches.
  uint64_t tolerance_ns{50000000};
};

/**
 * @brief Read an MCAP file and align its joint/camera streams into a frame sequence.
 *
 * Decodes the embedded dataset_info metadata, auto-detects leader/follower joint
 * streams by topic name (falling back to single-robot mode), parses all joint and
 * odometry messages, and produces one AlignedFrame per dataset row via
 * nearest-timestamp matching. Camera frames are matched the same way: each row records
 * the nearest frame per camera in CameraInfo::row_source_index, so images and joint
 * states in a row share an instant rather than a position. Rows where any stream or
 * camera has no sample within tolerance are dropped. Camera frames are NOT decoded
 * here; call extract_camera_images() for that.
 *
 * Camera keys keep the name the recording gave them.
 *
 * @param mcap_file Path to the input MCAP file.
 * @param episode_index Zero-based episode index to stamp on the episode.
 * @param out Output episode (overwritten on success).
 * @param channels Output channel maps (reused by extract_camera_images()).
 * @param signals Selects which decoded joint and base signals enter the row vectors.
 * @param alignment Row rate and match tolerance used to build the frame sequence.
 * @return true on success; false on a fatal error (message logged to stderr).
 */
bool load_aligned_episode(
  const std::string& mcap_file,
  int episode_index,
  AlignedEpisode& out,
  McapChannelMap& channels,
  const DatasetSignalOptions& signals = {},
  const AlignmentOptions& alignment = {});

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__MCAP_DATASET_LOADER_HPP_
