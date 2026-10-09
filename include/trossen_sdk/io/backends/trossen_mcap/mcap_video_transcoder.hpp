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

/// @brief Output-file settings for a transcode run.
struct VideoTranscodeOptions {
  /// @brief Chunk compression for the output file: "zstd", "lz4" or "" for none.
  std::string compression{"zstd"};
  /// @brief Overwrite the output file when it already exists.
  bool overwrite{false};
};

/// @brief What one transcode run produced.
struct VideoTranscodeStats {
  /// @brief Messages copied through untouched (joint states, odometry, frame meta, ...).
  size_t messages_copied{0};
  /// @brief Camera streams copied through untouched, because they already held video.
  size_t cameras_passthrough{0};
};

/**
 * @brief Copy one recording's channels and messages into a new file.
 *
 * Every channel is advertised in the output, and every message is written with its log time
 * unchanged. Camera topics are copied in the form they were stored.
 *
 * @param input Path to the recording to read.
 * @param output Path to the recording to write; must differ from `input`.
 * @param options Output-file settings.
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
