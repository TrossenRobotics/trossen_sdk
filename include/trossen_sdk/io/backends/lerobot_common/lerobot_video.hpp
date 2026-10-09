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
#include <vector>

#include <opencv2/core.hpp>

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

/**
 * @brief Wrap an already-compressed Annex B stream in mp4 without re-encoding.
 *
 * The bitstream is copied through verbatim (`-c copy`), so the recorded frames reach
 * LeRobot exactly as captured. Timestamps are generated at `fps` because an elementary
 * stream carries none.
 *
 * @param annexb Input elementary stream (`.h264` / `.hevc`).
 * @param frame_count Expected frame count, checked against the muxed result.
 * @param fps Frame rate stamped on the frames.
 * @param out_mp4 Output mp4.
 * @return true when the video holds exactly frame_count frames.
 */
bool remux_episode_video(
  const std::filesystem::path& annexb, size_t frame_count, double fps,
  const std::filesystem::path& out_mp4);

/**
 * @brief Decode evenly spaced frames from an mp4 for image statistics.
 *
 * Every frame is decoded and an evenly spaced sample of the frames from `first_frame`
 * is kept, as sample_images() loads them: CV_32FC3, BGR, scaled to [0, 1].
 *
 * @param mp4 Video to sample.
 * @param first_frame Index of the first frame to sample, the one row 0 plays.
 * @param frame_count Number of frames to sample from, starting at `first_frame`.
 * @param tmp_dir Scratch directory for the extracted stills.
 * @return Sampled frames (empty on failure; stats degrade rather than fail).
 */
std::vector<cv::Mat> sample_video_frames(
  const std::filesystem::path& mp4, size_t first_frame, size_t frame_count,
  const std::filesystem::path& tmp_dir);

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_VIDEO_HPP_
