/**
 * @file lerobot_video.hpp
 * @brief Per-episode video helpers shared by the LeRobot v2.1 and v3.0 converters.
 *
 * They run ffmpeg and ffprobe, which must be on PATH, and check every video they write
 * against the frame count the episode's rows expect.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_VIDEO_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_VIDEO_HPP_

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace trossen::io::backends {

/// @brief File name pattern of extracted color frames, numbered from 0.
inline constexpr char kColorFramePattern[] = "image_%06d.jpg";

/// @brief Codec of re-encoded color video.
inline constexpr char kColorCodec[] = "av1";

/// @brief Pixel format of re-encoded color video.
inline constexpr char kColorPixFmt[] = "yuv420p";

/**
 * @brief Exit code of a process from its std::system or pclose status.
 * @return -1 when the process did not exit normally, such as when a signal killed it.
 */
int exit_code(int status);

/**
 * @brief Packets in the first video stream of a file, one per frame in an mp4.
 * @return -1 when ffprobe fails or prints no count.
 */
int64_t count_video_packets(const std::filesystem::path& video);

/**
 * @brief Encode an episode's color frames as AV1 video.
 *
 * @param image_dir Folder holding the frames, named by kColorFramePattern from 0.
 * @param frame_count Frames to encode, checked against the written video.
 * @param fps Frame rate of the frames and of the video.
 * @param encoder_threads Cores the encoder may use (SVT-AV1 lp=); 0 leaves it uncapped.
 * @param out_mp4 Output mp4.
 * @return true when the video holds exactly frame_count frames.
 */
bool encode_episode_video(
  const std::filesystem::path& image_dir, size_t frame_count, double fps, int encoder_threads,
  const std::filesystem::path& out_mp4);

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_VIDEO_HPP_
