/**
 * @file mcap_video_transcoder.hpp
 * @brief Re-encoding a raw-image TrossenMCAP recording into a compressed-video one.
 *
 * A recording made with `image_encoding: raw` stores camera frames as `foxglove.RawImage`,
 * one uncompressed frame per message; one made with `image_encoding: video` stores them as
 * `foxglove.CompressedVideo`. This converts a recording of the first kind into the second,
 * in place of re-recording it: raw color camera topics are decoded and re-encoded as H.264,
 * every other topic is copied through unchanged, and the file-level metadata records are
 * not copied.
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
  /// @brief Target bitrate for color streams.
  int bitrate_kbps{trossen::configuration::TROSSEN_MCAP_DEFAULT_VIDEO_BITRATE_KBPS};
  /// @brief Keyframe interval in frames. Small values keep single-frame reads cheap.
  int keyframe_interval{trossen::configuration::TROSSEN_MCAP_DEFAULT_VIDEO_KEYFRAME_INTERVAL};
  /// @brief Chunk compression for the output file: "zstd", "lz4" or "" for none. Any other
  /// name fails the run.
  std::string compression{"zstd"};
  /// @brief Overwrite the output file when it already exists.
  bool overwrite{false};
};

/// @brief What one transcode run produced.
struct VideoTranscodeStats {
  /// @brief Messages copied through untouched (joint states, odometry, frame meta, ...).
  size_t messages_copied{0};
  /// @brief Color frames re-encoded to H.264.
  size_t color_frames_encoded{0};
  /// @brief Camera streams converted from raw images to video.
  size_t cameras_transcoded{0};
  /// @brief True when the recording's metadata says its camera frames are already video, so
  /// nothing was written.
  bool already_compressed{false};
};

/**
 * @brief Transcode one recording's raw camera images into compressed video.
 *
 * Message log times are preserved exactly, so every stream keeps the timing the converter
 * and the visualizer align on. Camera frames keep their arrival order and count: one input
 * frame produces one output packet. A frame the encoder rejects aborts the run rather than
 * shifting every later frame's alignment.
 *
 * A read problem, or a message on a channel missing from the summary, fails the run. A
 * file without a summary section was cut short and is refused. A channel with no messages
 * is not in the output.
 *
 * Requires the SDK to be built with TROSSEN_ENABLE_VIDEO_ENCODE; without it this fails.
 *
 * A recording must come from SDK kMinToolVersion or newer (its `tool_version`). One whose
 * `image_encoding` metadata is "video" is already compressed: nothing is written,
 * `stats.already_compressed` is set, and the call succeeds.
 *
 * The output is written to `<output>.partial` and renamed onto `output` only once complete,
 * so a failed run leaves no partial file and an earlier output stays untouched.
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
