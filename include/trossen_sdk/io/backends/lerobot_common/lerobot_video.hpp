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
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace trossen::io::backends {

/// @brief File name pattern of extracted color frames, numbered from 0.
inline constexpr char kColorFramePattern[] = "image_%06d.jpg";

/// @brief Codec of re-encoded color video.
inline constexpr char kColorCodec[] = "av1";

/// @brief Pixel format of re-encoded color video.
inline constexpr char kColorPixFmt[] = "yuv420p";

/// @brief File decode_video_rows() writes a depth camera's rows to, as gray12le codes.
inline constexpr char kDepthCodesFile[] = "depth_codes.gray12le";

/**
 * @brief Exit code of a process from its std::system or pclose status.
 *
 * @param status Wait status returned by std::system or pclose.
 * @return The exit code, or -1 when the process did not exit normally, such as when a
 *   signal killed it.
 */
int exit_code(int status);

/**
 * @brief Packets in the first video stream of a file, one per frame in an mp4.
 *
 * @param video Video file to probe.
 * @return The packet count, or -1 when ffprobe fails or prints no count.
 */
int64_t count_video_packets(const std::filesystem::path& video);

/**
 * @brief Duration of the first video stream of a file in seconds, read by ffprobe.
 *
 * The stream duration counts in the track's own timescale, while the container duration
 * can be rounded to milliseconds.
 * @param video File to probe.
 * @return -1 when ffprobe fails or prints no duration.
 */
double video_duration_s(const std::filesystem::path& video);

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
 * @brief Write a video camera's frames one per row by decoding its stream.
 *
 * A camera recorded at another rate than the rows cannot be stream-copied: its video holds
 * one frame per recorded frame, while LeRobot plays one per row. Recorded frame
 * `row_source_index[r]` is written for row r, so a slower camera's frames repeat and a
 * faster camera's are skipped, as LeRobot's recorder does.
 *
 * An H.264 (color) camera's rows are written to `dir` as kColorFramePattern JPEGs, the input
 * of encode_episode_video(). An H.265 (depth) camera holds 12-bit codes, written in row
 * order to `dir / kDepthCodesFile` as gray12le, the input of the lossless depth encode.
 *
 * @param annexb The camera's Annex B stream (extract_camera_video()).
 * @param storage The camera's recorded storage, "h264" or "h265".
 * @param width Frame width.
 * @param height Frame height.
 * @param row_source_index Recorded frame each row plays (CameraInfo::row_source_index), in
 *   increasing order.
 * @param dir Output folder.
 * @return false (reason on stderr) when decoding fails or ends before some row's frame.
 */
bool decode_video_rows(
  const std::filesystem::path& annexb, const std::string& storage, int width, int height,
  const std::vector<size_t>& row_source_index, const std::filesystem::path& dir);

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
