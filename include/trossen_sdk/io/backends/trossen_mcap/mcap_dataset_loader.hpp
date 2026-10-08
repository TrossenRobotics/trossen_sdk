/**
 * @file mcap_dataset_loader.hpp
 * @brief Reading a TrossenMCAP recording into an AlignedEpisode.
 *
 * The reader half of the TrossenMCAP backend: it decodes a finished recording, detects
 * leader/follower joint streams, and nearest-timestamp aligns them into one frame
 * sequence. Camera frames are matched per row and extracted on demand, either decoded to
 * images or copied out as an already-compressed video stream.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__MCAP_DATASET_LOADER_HPP_
#define TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__MCAP_DATASET_LOADER_HPP_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>

#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_episode.hpp"

namespace trossen::io::backends {

/// @brief Channel id as stored in an MCAP file; the same type as mcap::ChannelId, kept
/// here so this header does not expose the MCAP library.
using McapChannelId = std::uint16_t;

/// @brief Maps from MCAP channel id to semantic stream, populated during detection.
struct McapChannelMap {
  /// @brief Joint-state channel id to stream id (e.g. "leader", "follower_left").
  std::map<McapChannelId, std::string> joint_channels;
  /// @brief Image channel id to camera name (e.g. "cam_high").
  std::map<McapChannelId, std::string> camera_channels;
  /// @brief Mobile-base odometry channel (valid only when has_mobile_base is true).
  McapChannelId mobile_base_channel_id{0};
  /// @brief Whether a mobile-base odometry channel was found.
  bool has_mobile_base{false};
};

/// @brief Timing parameters for nearest-timestamp alignment.
struct AlignmentOptions {
  /// @brief Rate the output rows are generated at, in frames per second.
  double fps{DEFAULT_DATASET_FPS};
  /// @brief Largest gap, in nanoseconds, allowed between a row and the sample it matches.
  uint64_t tolerance_ns{DEFAULT_MATCH_TOLERANCE_NS};
};

/**
 * @brief Read an MCAP file and align its joint/camera streams into a frame sequence.
 *
 * Not implemented yet: returns false.
 *
 * @param mcap_file Path to the input MCAP file.
 * @param episode_index Zero-based episode index to stamp on the episode.
 * @param out Output episode (overwritten on success).
 * @param channels Output channel maps (reused by extract_camera_images()).
 * @param alignment Row rate and match tolerance used to build the frame sequence.
 * @return true on success; false on a fatal error (message logged to stderr).
 */
bool load_aligned_episode(
  const std::string& mcap_file,
  int episode_index,
  AlignedEpisode& out,
  McapChannelMap& channels,
  const AlignmentOptions& alignment = {});

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__MCAP_DATASET_LOADER_HPP_
