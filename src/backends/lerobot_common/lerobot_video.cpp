/**
 * @file lerobot_video.cpp
 * @brief Implementation of the shared per-episode video helpers.
 */

#include "trossen_sdk/io/backends/lerobot_common/lerobot_video.hpp"

#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "trossen_sdk/io/backend_utils.hpp"

namespace trossen::io::backends {

namespace fs = std::filesystem;

namespace {

/// @brief SVT-AV1 constant rate factor for color video.
constexpr int kAv1Crf = 30;
/// @brief Frames between keyframes in color video.
constexpr int kAv1KeyframeInterval = 30;
/// @brief SVT-AV1 speed preset for color video.
constexpr int kAv1Preset = 6;

}  // namespace

int exit_code(int status) {
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int64_t count_video_packets(const fs::path& video) {
  std::ostringstream probe;
  probe << "ffprobe -v error -count_packets -select_streams v:0 "
        << "-show_entries stream=nb_read_packets -of default=nw=1:nk=1 " << video.string();
  FILE* pipe = popen(probe.str().c_str(), "r");
  if (!pipe) return -1;
  char buf[64] = {0};
  const bool read_ok = std::fgets(buf, sizeof(buf), pipe) != nullptr;
  const int status = pclose(pipe);
  char* end = nullptr;
  const int64_t count = read_ok ? std::strtoll(buf, &end, 10) : -1;
  if (status != 0 || !read_ok || end == buf || count < 0) return -1;
  return count;
}

bool encode_episode_video(
  const fs::path& image_dir, size_t frame_count, double fps, int encoder_threads,
  const fs::path& out_mp4)
{
  const fs::path input_pattern = image_dir / kColorFramePattern;
  // SVT_LOG=1 limits SVT-AV1's own log, which -loglevel does not reach, to errors.
  std::ostringstream cmd;
  cmd << "SVT_LOG=1 ffmpeg -y -loglevel error -framerate " << fps << " -start_number 0"
      << " -i " << input_pattern.string() << " -frames:v " << frame_count
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
  cmd << " -pix_fmt " << kColorPixFmt << " -r " << fps << " " << out_mp4.string();
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
      << annexb.string() << " -vsync passthrough -f rawvideo -pix_fmt "
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

}  // namespace trossen::io::backends
