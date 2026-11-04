/**
 * @file lerobot_video.cpp
 * @brief Implementation of the shared per-episode video helpers.
 */

#include "trossen_sdk/io/backends/lerobot_common/lerobot_video.hpp"

#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>

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

}  // namespace trossen::io::backends
