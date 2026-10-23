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

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_episode.hpp"

namespace trossen::io::backends {

/// @brief Channel id as stored in an MCAP file; the same type as mcap::ChannelId, kept
/// here so this header does not expose the MCAP library.
using McapChannelId = std::uint16_t;

/// @brief Maps from MCAP channel id to stream, filled from the recording's dataset_info.
struct McapChannelMap {
  /// @brief Joint-state channel id to stream id (e.g. "leader", "follower_left").
  std::map<McapChannelId, std::string> joint_channels;
  /// @brief Image channel id to camera name (e.g. "cam_high").
  std::map<McapChannelId, std::string> camera_channels;
  /// @brief Camera name to how its frames are stored: "raw", "h264" or "h265".
  std::map<std::string, std::string> camera_storage;
  /// @brief Mobile-base odometry channel (valid only when has_mobile_base is true).
  McapChannelId mobile_base_channel_id{0};
  /// @brief Whether a mobile-base odometry channel was found.
  bool has_mobile_base{false};
};

/// @brief Timing parameters for nearest-timestamp alignment.
struct AlignmentOptions {
  /// @brief Rate the output rows are generated at, in frames per second. Must be a whole
  /// number from 1 to 1000000000; otherwise load_aligned_episode() returns false.
  double fps{DEFAULT_DATASET_FPS};
  /// @brief Largest gap, in nanoseconds, allowed between a row and the sample it matches.
  uint64_t tolerance_ns{DEFAULT_MATCH_TOLERANCE_NS};
  /// @brief Longest single gap, in milliseconds, between two kept rows; an episode with a
  ///        longer one is rejected.
  double max_single_gap_ms{DEFAULT_MAX_SINGLE_GAP_MS};
  /// @brief Largest share of the grid's rows, from 0 to 1, dropped between the first and
  ///        last kept row; past it the episode is rejected.
  double max_gap_fraction{DEFAULT_MAX_GAP_FRACTION};
  /// @brief Leave depth cameras out of the episode before they are checked or aligned, for
  ///        a format that cannot store depth. A depth camera then cannot reject the episode
  ///        or cost it rows.
  bool skip_depth_cameras{false};
};

/// @brief Returned by nearest_sample_index() when no entry is within the tolerance.
inline constexpr size_t NO_SAMPLE = std::numeric_limits<size_t>::max();

/**
 * @brief Index of the entry nearest a target time in a time-ordered sequence.
 *
 * Both neighbors of the target are compared, so a row pairs with a later sample when that
 * one is closer; on an equal distance the earlier entry wins. `cursor` is the search start
 * and is left on the last entry at or before the target, so successive calls with
 * increasing targets each scan forward from where the previous one stopped.
 *
 * @tparam StampAt Callable taking an index and returning that entry's time in nanoseconds.
 * @param count Number of entries.
 * @param stamp_at Returns entry i's time, in the same clock as `target`.
 * @param target Time to match, in nanoseconds.
 * @param tolerance_ns Largest distance, in nanoseconds, at which the nearest entry is accepted.
 * @param cursor Search cursor; only moves forward.
 * @return Index of the nearest entry, or NO_SAMPLE when there are no entries or the nearest
 *   is further than `tolerance_ns` from `target`.
 */
template <typename StampAt>
size_t nearest_sample_index(
  size_t count, const StampAt& stamp_at, uint64_t target, uint64_t tolerance_ns, size_t& cursor)
{
  if (cursor >= count) return NO_SAMPLE;
  while (cursor + 1 < count && stamp_at(cursor + 1) <= target) ++cursor;
  auto distance = [target](uint64_t ts) { return target > ts ? target - ts : ts - target; };
  size_t best = cursor;
  if (cursor + 1 < count && distance(stamp_at(cursor + 1)) < distance(stamp_at(cursor))) {
    best = cursor + 1;
  }
  return distance(stamp_at(best)) > tolerance_ns ? NO_SAMPLE : best;
}

/**
 * @brief First entry whose time is earlier than the entry before it.
 *
 * nearest_sample_index() moves its cursor forward only, so a stream must be in time order
 * for its rows to match correctly. Equal consecutive times are in order.
 *
 * @tparam StampAt Callable taking an index and returning that entry's time in nanoseconds.
 * @param count Number of entries.
 * @param stamp_at Returns entry i's time.
 * @return Index of the first entry that steps backwards, or no value when the sequence is in
 *   order.
 */
template <typename StampAt>
std::optional<size_t> first_backward_step(size_t count, const StampAt& stamp_at)
{
  for (size_t i = 1; i < count; ++i) {
    if (stamp_at(i) < stamp_at(i - 1)) return i;
  }
  return std::nullopt;
}

/// @brief The span of time in which every stream added so far has data.
struct StreamWindow {
  /// @brief Latest first sample among the streams, in nanoseconds of log time.
  uint64_t start{0};
  /// @brief Earliest last sample among the streams, in nanoseconds of log time.
  uint64_t end{std::numeric_limits<uint64_t>::max()};
  /// @brief Stream that set `start`, for reporting a window that closed.
  std::string last_to_start;
  /// @brief Stream that set `end`, for reporting a window that closed.
  std::string first_to_end;

  /**
   * @brief Narrow the window to a stream's first and last sample times.
   *
   * @param name Stream name, kept when it sets an edge.
   * @param first Stream's first sample time, in nanoseconds of log time.
   * @param last Stream's last sample time, in nanoseconds of log time.
   */
  void narrow(const std::string& name, uint64_t first, uint64_t last);

  /// @return true when the window spans a positive length of time.
  bool is_open() const;
};

/**
 * @brief Row times on a uniform grid from `start`, one per `period_ns`, up to `end`.
 *
 * @param start First row time, in nanoseconds.
 * @param end Last time a row may fall on, in nanoseconds; must be at least `start`.
 * @param period_ns Spacing between rows, in nanoseconds; must be above zero.
 * @return Row times `start + k * period_ns` for every k that stays at or before `end`.
 */
std::vector<uint64_t> row_grid(uint64_t start, uint64_t end, uint64_t period_ns);

/// @brief Rows dropped between the first and last kept row, measured against the limits in
///        AlignmentOptions.
struct GapSummary {
  /// @brief Rows dropped across every gap.
  size_t dropped_rows{0};
  /// @brief Length of the longest gap, in rows.
  size_t longest_gap_rows{0};
  /// @brief `dropped_rows` as a share of the grid's rows, from 0 to 1.
  double dropped_fraction{0.0};
  /// @brief Whether the longest gap is over AlignmentOptions::max_single_gap_ms.
  bool gap_too_long{false};
  /// @brief Whether `dropped_fraction` is over AlignmentOptions::max_gap_fraction.
  bool too_many_gaps{false};
};

/**
 * @brief Total and longest mid-episode gap, and whether either is over its limit.
 *
 * A gap exactly at a limit is accepted.
 *
 * @param gap_lengths Length of each gap, in rows.
 * @param grid_rows Number of rows on the episode's grid; must be above zero.
 * @param row_period_ns Spacing between rows, in nanoseconds.
 * @param alignment Supplies `max_single_gap_ms` and `max_gap_fraction`.
 * @return The totals and the limit checks.
 */
GapSummary summarize_gaps(
  const std::vector<size_t>& gap_lengths, size_t grid_rows, uint64_t row_period_ns,
  const AlignmentOptions& alignment);

/**
 * @brief Duration of a run of rows, in milliseconds.
 *
 * @param rows Number of rows.
 * @param row_period_ns Spacing between rows, in nanoseconds.
 * @return The duration, in milliseconds.
 */
double rows_to_ms(size_t rows, uint64_t row_period_ns);

/**
 * @brief Largest distance, in nanoseconds, at which a camera frame is matched to a row.
 *
 * A camera slower than the rows gets a wider window, so each of its frames fills the rows
 * nearest it.
 *
 * @param camera_fps The camera's recorded rate.
 * @param row_fps The row rate (`alignment.fps`).
 * @param tolerance_ns `alignment.tolerance_ns`.
 * @return `tolerance_ns`, plus half of `1/camera_fps - 1/row_fps` when the camera is slower.
 */
uint64_t camera_match_window_ns(double camera_fps, double row_fps, uint64_t tolerance_ns);

/**
 * @brief Remove every depth camera from a channel map.
 *
 * load_aligned_episode() calls this when AlignmentOptions::skip_depth_cameras is set, before
 * any camera is checked or aligned. Each removed camera is named in a warning on stderr.
 *
 * @param channels Channel maps to filter in place; only camera_channels changes.
 * @param dataset_info The recording's dataset_info metadata, which describes every camera.
 * @throws nlohmann::json::exception if a camera has no boolean `is_depth_map` in dataset_info.
 */
void drop_depth_cameras(McapChannelMap& channels, const nlohmann::json& dataset_info);

/**
 * @brief Read an MCAP file and align its streams into one row per tick of `alignment.fps`.
 *
 * Rows span the time every joint stream, camera and mobile base has data.
 * Each row holds the sample nearest its time from every joint, odometry and camera
 * stream; a camera's match is stored in CameraInfo::row_source_index, not decoded (call
 * extract_camera_images() for images). Leader and follower streams are found by their
 * `role`. A row with no sample within `alignment.tolerance_ns` for any stream or camera
 * is dropped. Camera keys keep the names the recording gave them.
 *
 * Fails on a recording it cannot align, or whose dropped rows exceed the gap limits in
 * `alignment`.
 *
 * @param mcap_file Path to the input MCAP file.
 * @param episode_index Zero-based episode index to stamp on the episode.
 * @param out Output episode (overwritten on success).
 * @param channels Output channel maps (reused by extract_camera_images()).
 * @param alignment Row rate, match tolerance and allowed mid-episode gap.
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
 * For every row, writes the frame chosen by CameraInfo::row_source_index as
 * `image_%06d.jpg` (JPEG at DEFAULT_JPEG_QUALITY), or as a 16-bit `image_%06d.png` in
 * millimeters for depth. Files are numbered by row, so a frame matched by two rows is written
 * twice. Handles cameras stored as `foxglove.RawImage`; for `foxglove.CompressedVideo` use
 * extract_camera_video().
 *
 * @param mcap_file Path to the input MCAP file.
 * @param channels Channel maps from load_aligned_episode() on the same file.
 * @param episode Aligned episode supplying the per-row frame selection.
 * @param dir_for Called once per camera; returns the directory for its frames, which it must
 *        create.
 * @param out_counts Output per-camera written-frame counts (one per dataset row).
 * @return true on success; false if a matched frame could not be decoded or written, or on
 *         a fatal read error (message logged to stderr). On failure the images this call
 *         wrote are removed and @p out_counts is empty.
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
 * Handles camera channels whose schema is `foxglove.CompressedVideo`. Each
 * camera's message payloads are appended, in file order, to a single Annex B
 * elementary stream under `dir_for(camera_name)`. Nothing is decoded: the point
 * is to let the caller remux with `ffmpeg -c copy`.
 *
 * Cameras stored as `foxglove.RawImage` are ignored, so a raw recording yields no
 * streams and goes to extract_camera_images() instead.
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
 * @brief Source frames a camera recorded before the episode's first row.
 *
 * @param cam Camera with its per-row frame selections.
 * @return Index of the frame matched to row 0, or 0 when no row matched a frame.
 */
size_t video_start_offset(const CameraInfo& cam);

/// @brief Most frames a row's image may be from the frame matched to it, about 100 ms at
/// 30 fps. A stream-copied camera further off than this fails the episode.
inline constexpr size_t MAX_VIDEO_DRIFT_FRAMES = 3;

/**
 * @brief Check that each stream-copied camera's rows play the frames matched to them
 *
 * Stream-copied video is written in recording order starting at the camera's first frame.
 * When the writer moves each video's start to the frame matched to row 0 (LeRobot v3 does
 * this through `from_timestamp`), row n plays frame n + offset; otherwise row n plays
 * frame n, and a nonzero offset is reported as the first image's lag.
 *
 * @param ep Aligned episode carrying the per-row frame selections.
 * @param video_streams Extracted video streams keyed by camera name.
 * @param start_offset_applied Whether the writer starts each video at the frame matched
 *   to row 0.
 * @return False if some row plays a frame more than MAX_VIDEO_DRIFT_FRAMES from the frame
 *   matched to it (message logged to stderr); true otherwise.
 */
bool check_video_alignment(
  const AlignedEpisode& ep, const std::map<std::string, CameraVideoStream>& video_streams,
  bool start_offset_applied);

/**
 * @brief Trim an aligned episode to the rows every video-mode camera covers.
 *
 * A compressed camera stream holds the camera's frames in arrival order, so a camera that
 * recorded fewer frames than the episode has rows leaves the last rows without video.
 * Trimming drops rows from the end and prints a warning naming the shortest camera and the
 * rows dropped. Raw-image cameras are not considered.
 *
 * @param ep Aligned episode to trim in place; a no-op if no camera is shorter than the
 *   episode already is.
 * @param video_streams Per-camera compressed video streams from extract_camera_video(),
 *   keyed by camera name (AlignedEpisode::cameras[i].name).
 * @param start_offset_applied Whether the writer starts each video at the frame matched
 *   to row 0, in which case the frames before it are never played and do not count.
 */
void clamp_episode_to_video_frame_counts(
  AlignedEpisode& ep, const std::map<std::string, CameraVideoStream>& video_streams,
  bool start_offset_applied);

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__MCAP_DATASET_LOADER_HPP_
