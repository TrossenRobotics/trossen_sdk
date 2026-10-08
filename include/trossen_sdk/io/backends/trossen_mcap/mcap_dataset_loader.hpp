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

/**
 * @brief Write each dataset row's matched camera frame from an MCAP file as an image file.
 *
 * Not implemented yet: returns false and writes nothing.
 *
 * @param mcap_file Path to the input MCAP file.
 * @param channels Channel maps from load_aligned_episode() on the same file.
 * @param episode Aligned episode supplying the per-row frame selection.
 * @param dir_for Called once per camera; returns the directory for its frames, which it must
 *        create.
 * @param out_counts Output per-camera written-frame counts (one per dataset row).
 * @return true on success; false if a matched frame could not be decoded or written, or on
 *         a fatal read error (message logged to stderr).
 */
bool extract_camera_images(
  const std::string& mcap_file,
  const McapChannelMap& channels,
  const AlignedEpisode& episode,
  const std::function<std::filesystem::path(const std::string& camera_name)>& dir_for,
  std::map<std::string, size_t>& out_counts);

/**
 * @brief Extract already-compressed camera video straight out of an MCAP.
 *
 * Not implemented yet: returns false and writes nothing.
 *
 * @param mcap_file Path to the input MCAP file.
 * @param channels Channel maps from load_aligned_episode().
 * @param dir_for Maps a camera name to a directory to write into (created by the callback).
 * @param out_streams Output per-camera video streams; empty when the recording has none.
 * @return true on success; false on a fatal read error (message logged to stderr).
 */
bool extract_camera_video(
  const std::string& mcap_file,
  const McapChannelMap& channels,
  const std::function<std::filesystem::path(const std::string& camera_name)>& dir_for,
  std::map<std::string, CameraVideoStream>& out_streams);

/**
 * @brief Trim an aligned episode to whatever every video-mode camera actually covers.
 *
 * Not implemented yet: leaves the episode unchanged.
 *
 * @param ep Aligned episode to trim in place; a no-op if no camera is shorter than the
 *   episode already is.
 * @param video_streams Per-camera compressed video streams from extract_camera_video(),
 *   keyed by camera name (AlignedEpisode::cameras[i].name).
 */
void clamp_episode_to_video_frame_counts(
  AlignedEpisode& ep, const std::map<std::string, CameraVideoStream>& video_streams);

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__MCAP_DATASET_LOADER_HPP_
