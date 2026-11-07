/**
 * @file lerobot_video.cpp
 * @brief Implementation of the shared per-episode video helpers.
 */

#include "trossen_sdk/io/backends/lerobot_common/lerobot_video.hpp"

#include <sys/wait.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_stats_utils.hpp"
#include "trossen_sdk/utils/shell_quote.hpp"

namespace trossen::io::backends {

namespace fs = std::filesystem;

namespace {

/// @brief SVT-AV1 constant rate factor for color video.
constexpr int kAv1Crf = 30;
/// @brief Frames between keyframes in color video.
constexpr int kAv1KeyframeInterval = 30;
/// @brief SVT-AV1 speed preset for color video.
constexpr int kAv1Preset = 6;
/// @brief ffmpeg JPEG quality of the stills sampled from a video (2 is near lossless).
constexpr int kSampleJpegQuality = 2;
/// @brief Stills sampled from each video for image statistics, roughly.
constexpr size_t kTargetSamples = 30;

/**
 * @brief First line ffprobe prints about the first video stream of a file.
 * @param options ffprobe options that select what to print.
 * @param video File to probe.
 * @return The line, or std::nullopt when ffprobe fails or prints nothing.
 */
std::optional<std::string> probe_video_stream(const std::string& options, const fs::path& video) {
  std::ostringstream probe;
  probe << "ffprobe -v error -select_streams v:0 " << options << " -of default=nw=1:nk=1 "
        << trossen::utils::shell_quote(video);
  FILE* pipe = popen(probe.str().c_str(), "r");
  if (!pipe) return std::nullopt;
  char buf[64] = {0};
  const bool read_ok = std::fgets(buf, sizeof(buf), pipe) != nullptr;
  const int status = pclose(pipe);
  if (status != 0 || !read_ok) return std::nullopt;
  return std::string(buf);
}

}  // namespace

int exit_code(int status) {
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int64_t count_video_packets(const fs::path& video) {
  const auto line =
    probe_video_stream("-count_packets -show_entries stream=nb_read_packets", video);
  if (!line) return -1;
  char* end = nullptr;
  const int64_t count = std::strtoll(line->c_str(), &end, 10);
  if (end == line->c_str() || count < 0) return -1;
  return count;
}

double video_duration_s(const fs::path& video) {
  const auto line = probe_video_stream("-show_entries stream=duration", video);
  if (!line) return -1.0;
  char* end = nullptr;
  const double duration = std::strtod(line->c_str(), &end);
  if (end == line->c_str() || duration < 0.0) return -1.0;
  return duration;
}

bool encode_episode_video(
  const fs::path& image_dir, size_t frame_count, double fps, int encoder_threads,
  const fs::path& out_mp4)
{
  const fs::path input_pattern = image_dir / kColorFramePattern;
  // SVT_LOG=1 limits SVT-AV1's own log, which -loglevel does not reach, to errors.
  std::ostringstream cmd;
  cmd << "SVT_LOG=1 ffmpeg -y -loglevel error -framerate " << fps << " -start_number 0"
      << " -i " << trossen::utils::shell_quote(input_pattern) << " -frames:v " << frame_count
      << " -c:v libsvtav1 -crf " << kAv1Crf << " -g " << kAv1KeyframeInterval << " -preset "
      << kAv1Preset;
  // SVT-AV1's level of parallelism. ffmpeg's -threads is silently ignored by this encoder,
  // so lp= is the only way to stop each concurrent encoder from sizing itself to the
  // whole machine.
  if (encoder_threads > 0) {
    cmd << " -svtav1-params lp=" << encoder_threads;
  }
  // The output rate matches the input rate; -frames:v counts output frames, so any other
  // rate would cut the video short or duplicate frames.
  cmd << " -pix_fmt " << kColorPixFmt << " -r " << fps << " "
      << trossen::utils::shell_quote(out_mp4);
  const int ret = std::system(cmd.str().c_str());
  if (ret != 0) {
    std::cerr << "Error: ffmpeg encode failed (exit " << exit_code(ret) << "): " << cmd.str()
              << "\n";
    return false;
  }

  // ffmpeg stops at the first missing image and still exits 0, so a short image sequence
  // gives a short video. Each encoded frame is one packet.
  const int64_t encoded = count_video_packets(out_mp4);
  if (encoded < 0 || static_cast<size_t>(encoded) != frame_count) {
    std::cerr << "Error: encoded " << out_mp4.filename().string() << " has " << encoded
              << " frames but the episode has " << frame_count << "\n";
    return false;
  }
  return true;
}

bool remux_episode_video(
  const fs::path& annexb, size_t frame_count, double fps, const fs::path& out_mp4)
{
  // -c copy: the bitstream goes through untouched. An elementary stream has no container
  // timestamps, so -r stamps them at `fps` and +genpts fills in the presentation timestamps
  // the mp4 muxer needs.
  std::ostringstream cmd;
  cmd << "ffmpeg -y -loglevel error -fflags +genpts -r " << fps << " -i "
      << trossen::utils::shell_quote(annexb) << " -c copy -movflags +faststart "
      << trossen::utils::shell_quote(out_mp4);
  const int ret = std::system(cmd.str().c_str());
  if (ret != 0) {
    std::cerr << "Error: ffmpeg remux failed (exit " << exit_code(ret) << "): " << cmd.str()
              << "\n";
    return false;
  }

  // A frame count that disagrees with the message count silently shifts image/state
  // alignment for every later frame. A stream copy writes one packet per frame, so the
  // packet count is the frame count. A count that cannot be read fails the remux.
  const int64_t muxed = count_video_packets(out_mp4);
  if (muxed < 0) {
    std::cerr << "Error: could not count the frames of remuxed video " << out_mp4.string()
              << "\n";
    return false;
  }
  if (static_cast<size_t>(muxed) != frame_count) {
    std::cerr << "Error: remuxed " << out_mp4.filename().string() << " has " << muxed
              << " frames but the recording had " << frame_count
              << " video messages; refusing to misalign frames against joint states\n";
    return false;
  }
  return true;
}

bool decode_video_rows(
  const fs::path& annexb, const std::string& storage, int width, int height,
  const std::vector<size_t>& row_source_index, const fs::path& dir)
{
  const bool depth = storage == "h265";
  const size_t frame_bytes =
    static_cast<size_t>(width) * static_cast<size_t>(height) * (depth ? 2 : 3);
  // passthrough keeps one output frame per decoded frame; the default would retime them.
  // -vsync is used because FFmpeg 4.4 (Ubuntu 22.04) has no -fps_mode.
  std::ostringstream cmd;
  cmd << "ffmpeg -loglevel error -f " << (depth ? "hevc" : "h264") << " -i "
      << trossen::utils::shell_quote(annexb) << " -vsync passthrough -f rawvideo -pix_fmt "
      << (depth ? "gray12le" : "bgr24") << " -";
  FILE* pipe = popen(cmd.str().c_str(), "r");
  if (pipe == nullptr) {
    std::cerr << "Error: could not start ffmpeg to decode " << annexb.string() << "\n";
    return false;
  }

  std::ofstream codes;
  if (depth) codes.open(dir / kDepthCodesFile, std::ios::binary);
  const std::vector<int> jpeg_params{cv::IMWRITE_JPEG_QUALITY, DEFAULT_JPEG_QUALITY};
  std::vector<unsigned char> frame(frame_bytes);
  bool ok = !depth || static_cast<bool>(codes);
  size_t row = 0;
  for (size_t source = 0; ok && row < row_source_index.size(); ++source) {
    if (std::fread(frame.data(), 1, frame_bytes, pipe) != frame_bytes) {
      std::cerr << "Error: " << annexb.filename().string() << " decodes to " << source
                << " frames, but row " << row << " plays frame " << row_source_index[row]
                << "\n";
      ok = false;
      break;
    }
    // Rows match frames in time order, so the rows that play this frame come next.
    for (; row < row_source_index.size() && row_source_index[row] == source; ++row) {
      if (depth) {
        codes.write(reinterpret_cast<const char*>(frame.data()),
                    static_cast<std::streamsize>(frame_bytes));
        continue;
      }
      const cv::Mat image(height, width, CV_8UC3, frame.data());
      char name[32];
      std::snprintf(name, sizeof(name), kColorFramePattern, static_cast<int>(row));
      if (!cv::imwrite((dir / name).string(), image, jpeg_params)) {
        std::cerr << "Error: failed to write " << (dir / name).string() << "\n";
        ok = false;
        break;
      }
    }
  }
  // Read to the end, so ffmpeg finishes rather than failing on a closed pipe.
  while (std::fread(frame.data(), 1, frame_bytes, pipe) == frame_bytes) {}
  const int status = pclose(pipe);
  if (ok && exit_code(status) != 0) {
    std::cerr << "Error: ffmpeg decode failed (exit " << exit_code(status) << "): " << cmd.str()
              << "\n";
    ok = false;
  }
  if (depth) {
    codes.close();
    if (ok && !codes) {
      std::cerr << "Error: failed to write " << (dir / kDepthCodesFile).string() << "\n";
      ok = false;
    }
  }
  return ok;
}

std::vector<cv::Mat> sample_video_frames(
  const fs::path& mp4, size_t first_frame, size_t frame_count, const fs::path& tmp_dir)
{
  if (frame_count == 0) return {};

  // The folder is emptied first so stills left by an earlier run are not counted.
  const fs::path sample_dir = tmp_dir / "samples";
  std::error_code ec;
  fs::remove_all(sample_dir, ec);
  if (!ec) fs::create_directories(sample_dir, ec);
  if (ec) {
    std::cerr << "Warning: could not prepare " << sample_dir.string() << ": " << ec.message()
              << "; no image stats are sampled from " << mp4.string() << "\n";
    return {};
  }

  // The select filter decodes every frame and keeps every stride-th one from
  // `first_frame` through the last of the `frame_count` frames. Passthrough
  // writes each kept frame once instead of duplicating frames to fill the gaps;
  // -vsync is used because FFmpeg 4.4 (Ubuntu 22.04) has no -fps_mode.
  const size_t stride = std::max<size_t>(1, frame_count / kTargetSamples);
  const size_t last_frame = first_frame + frame_count - 1;

  std::ostringstream cmd;
  cmd << "ffmpeg -y -loglevel error -i " << trossen::utils::shell_quote(mp4)
      << " -vf \"select='between(n\\," << first_frame << "\\," << last_frame
      << ")*not(mod(n-" << first_frame << "\\," << stride << "))'\" -vsync passthrough -q:v "
      << kSampleJpegQuality << " " << trossen::utils::shell_quote(sample_dir / "sample_%04d.jpg");
  const int ret = std::system(cmd.str().c_str());
  if (ret != 0) {
    std::cerr << "Warning: could not sample frames from " << mp4.string() << " (exit "
              << exit_code(ret) << "); image stats for this camera will be based on fewer "
              << "frames\n";
  }

  std::vector<fs::path> paths;
  if (fs::exists(sample_dir)) {
    for (const auto& entry : fs::directory_iterator(sample_dir)) {
      if (entry.is_regular_file() && entry.path().extension() == ".jpg") {
        paths.push_back(entry.path());
      }
    }
  }
  std::sort(paths.begin(), paths.end());
  std::vector<cv::Mat> samples = sample_images(paths);

  fs::remove_all(sample_dir, ec);
  return samples;
}

}  // namespace trossen::io::backends
