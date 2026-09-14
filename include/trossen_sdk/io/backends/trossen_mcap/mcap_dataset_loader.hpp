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

/**
 * @brief Decode the row-matched camera frames from an MCAP file into caller-chosen directories.
 *
 * Re-opens the MCAP file and writes, for every dataset row, that row's matched frame
 * (CameraInfo::row_source_index) as `image_%06d.jpg` (JPEG quality 95, or 16-bit
 * `image_%06d.png` for depth under the native schema) into the directory returned by
 * `dir_for(camera_name)`. Output frames are numbered by row, so frame *k* of the encoded
 * video is the observation belonging to data row *k*; a source frame matched by two
 * consecutive rows is written twice. The callback is invoked once per camera and is
 * responsible for creating the dir.
 *
 * Handles camera channels whose schema is `foxglove.RawImage`. Cameras stored as
 * `foxglove.CompressedVideo` are skipped here; use extract_camera_video() for those. A
 * recording may mix the two (color as video, depth as raw), so a caller should consult both.
 *
 * Both passes read the same file through the same reader, so a camera's arrival order here
 * is identical to the one load_aligned_episode() indexed into.
 *
 * @param mcap_file Path to the input MCAP file.
 * @param channels Channel maps from load_aligned_episode().
 * @param episode Aligned episode supplying the per-row frame selection.
 * @param dir_for Maps a camera name to the directory its frames are written to.
 * @param out_counts Output per-camera written-frame counts (one per dataset row).
 * @param native_schema Preserve 16-bit depth losslessly as PNG instead of 8-bit JPEG.
 * @return true on success; false if a matched frame could not be decoded or written, or on
 *         a fatal read error (message logged to stderr).
 */
bool extract_camera_images(
  const std::string& mcap_file,
  const McapChannelMap& channels,
  const AlignedEpisode& episode,
  const std::function<std::filesystem::path(const std::string& camera_name)>& dir_for,
  std::map<std::string, size_t>& out_counts,
  bool native_schema = false);

/**
 * @brief Extract already-compressed camera video straight out of an MCAP.
 *
 * Handles camera channels whose schema is `foxglove.CompressedVideo`. Each
 * camera's message payloads are appended, in log-time order, to a single Annex B
 * elementary stream under `dir_for(camera_name)`. Nothing is decoded: the point
 * is to let the caller remux with `ffmpeg -c copy`.
 *
 * Cameras stored as `foxglove.RawImage` are ignored here; use
 * extract_camera_images() for those. A recording may legitimately mix the two
 * (color as video, depth as raw), so a caller should consult both.
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
 * Compressed camera streams are remuxed verbatim (extract_camera_video(), arrival order,
 * not row-aligned), so a camera that free-ran short ends up with fewer frames than the
 * joint-aligned episode has rows: lerobot then queries that video by row timestamp and
 * walks off the end. Raw-image cameras always yield exactly one frame per row, because
 * extract_camera_images() writes each row's matched source frame and writes it again when
 * two rows match the same one, so only video-mode cameras are considered here.
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
