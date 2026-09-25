/**
 * @file mcap_video_transcoder.hpp
 * @brief Re-encoding a raw-image TrossenMCAP recording into a compressed-video one.
 *
 * Older recordings store camera frames as `foxglove.RawImage`, one uncompressed frame per
 * message. The recorder can now store them as `foxglove.CompressedVideo` instead. This
 * converts an existing recording of the first kind into the second, in place of
 * re-recording it: camera topics are decoded and re-encoded (H.264 color, lossless H.265
 * for 12-bit depth), and every other topic, along with the file-level metadata records, is
 * copied through unchanged.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__MCAP_VIDEO_TRANSCODER_HPP_
#define TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__MCAP_VIDEO_TRANSCODER_HPP_

#include <cstddef>
#include <filesystem>
#include <string>

#include "trossen_sdk/configuration/types/backends/trossen_mcap_backend_config.hpp"

namespace trossen::io::backends {

/// @brief Encoder settings for a transcode run, mirroring the recorder's video options.
struct VideoTranscodeOptions {
  /// @brief Preferred encoder name, or "auto" to probe hardware then software.
  std::string encoder{trossen::configuration::TROSSEN_MCAP_DEFAULT_VIDEO_ENCODER};
  /// @brief Target bitrate for color streams; depth is always lossless.
  int bitrate_kbps{trossen::configuration::TROSSEN_MCAP_DEFAULT_VIDEO_BITRATE_KBPS};
  /// @brief Keyframe interval in frames. Small values keep single-frame reads cheap.
  int keyframe_interval{trossen::configuration::TROSSEN_MCAP_DEFAULT_VIDEO_KEYFRAME_INTERVAL};
  /// @brief Chunk compression for the output file: "zstd", "lz4" or "" for none.
  std::string compression{"zstd"};
  /// @brief Overwrite the output file when it already exists.
  bool overwrite{false};

  /**
   * @brief Bring the recording's own metadata in line with its filename.
   *
   * Recordings written before the SDK named episodes by UUID carry an
   * `episode_index` that was only ever unique inside the folder they were
   * recorded into, and name the prompt `task` rather than `task_description`.
   * Once such files sit in one flat directory the index identifies nothing, so
   * the transcode writes `episode_id` from the filename and renames the prompt.
   * `episode_index` is left as recorded; it is what the recorder observed.
   */
  bool rewrite_episode_metadata{true};

  /// @brief Episode id to write; empty means the output file's own stem.
  std::string episode_id;

  /**
   * @brief Copy the recording as it is, changing only its metadata.
   *
   * Camera frames are passed through in whatever form they were stored, so a
   * raw recording stays raw. For fixing the metadata of files that are keeping
   * their original format, at the cost of rewriting each one.
   */
  bool metadata_only{false};
};

/// @brief What one transcode run produced.
struct VideoTranscodeStats {
  /// @brief Messages copied through untouched (joint states, odometry, frame meta, ...).
  size_t messages_copied{0};
  /// @brief Color frames re-encoded to H.264.
  size_t color_frames_encoded{0};
  /// @brief Depth frames quantized and re-encoded to lossless H.265.
  size_t depth_frames_encoded{0};
  /// @brief Camera streams converted from raw images to video.
  size_t cameras_transcoded{0};
  /// @brief Camera streams copied through untouched, because they already held
  ///        video or because only the metadata is being changed.
  size_t cameras_passthrough{0};
  /// @brief File-level metadata records copied.
  size_t metadata_records_copied{0};
  /// @brief Whether the recording's metadata was rewritten to match its name.
  bool metadata_rewritten{false};
};

/**
 * @brief Transcode one recording's raw camera images into compressed video.
 *
 * Message log times are preserved exactly, so every stream keeps the timing the converter
 * and the visualizer align on. Camera frames keep their arrival order and count: one input
 * frame produces one output packet, which is what lets a reader pair frame n of the video
 * with the nth message of the stream's `/meta` topic. A frame the encoder rejects aborts
 * the run rather than shifting every later frame's alignment.
 *
 * Camera channels already carrying `foxglove.CompressedVideo` are copied verbatim, so a
 * recording that mixes the two storage formats converts cleanly and re-running the tool on
 * its own output is a no-op.
 *
 * Requires the SDK to be built with TROSSEN_ENABLE_VIDEO_ENCODE; without it this fails.
 *
 * @param input Path to the recording to read.
 * @param output Path to the recording to write; must differ from `input`.
 * @param options Encoder and output-file settings.
 * @param stats Output counters describing the run (valid only on success).
 * @return true on success; false on any fatal error (reason logged to stderr).
 */
bool transcode_images_to_video(
  const std::filesystem::path& input,
  const std::filesystem::path& output,
  const VideoTranscodeOptions& options,
  VideoTranscodeStats& stats);

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP__MCAP_VIDEO_TRANSCODER_HPP_
