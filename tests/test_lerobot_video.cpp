/**
 * @file test_lerobot_video.cpp
 * @brief Unit tests for the per-episode video helpers shared by the LeRobot converters.
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <opencv2/imgcodecs.hpp>

#include "trossen_sdk/io/backends/lerobot_common/lerobot_video.hpp"

namespace fs = std::filesystem;
using trossen::io::backends::count_video_packets;
using trossen::io::backends::encode_episode_video;
using trossen::io::backends::kColorFramePattern;

namespace {

/// @brief True when ffmpeg and ffprobe are both on PATH.
bool have_ffmpeg() {
  return std::system("command -v ffmpeg >/dev/null 2>&1 && command -v ffprobe >/dev/null 2>&1") ==
         0;
}

/// @brief An emptied folder under the temp directory.
fs::path fresh_dir(const std::string& name) {
  const fs::path dir = fs::temp_directory_path() / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

/// @brief Frame rate of the first video stream as ffprobe prints it, such as "15/1".
std::string probe_frame_rate(const fs::path& mp4) {
  const std::string cmd =
    "ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate "
    "-of default=nw=1:nk=1 '" + mp4.string() + "'";
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return {};
  char buf[64] = {0};
  const bool read_ok = std::fgets(buf, sizeof(buf), pipe) != nullptr;
  pclose(pipe);
  std::string rate = read_ok ? buf : "";
  while (!rate.empty() && (rate.back() == '\n' || rate.back() == '\r')) rate.pop_back();
  return rate;
}

/// @brief Write color frame `index` under kColorFramePattern.
void write_color_frame(const fs::path& dir, int index) {
  char name[32];
  std::snprintf(name, sizeof(name), kColorFramePattern, index);
  ASSERT_TRUE(cv::imwrite((dir / name).string(),
                          cv::Mat(64, 64, CV_8UC3, cv::Scalar(10 * index, 100, 200))));
}

}  // namespace

TEST(LeRobotVideo, ColorEncodeWritesEveryFrame) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_color_encode_test");
  for (int i = 0; i < 3; ++i) write_color_frame(dir, i);
  const fs::path mp4 = dir / "color.mp4";

  ASSERT_TRUE(encode_episode_video(dir, 3, 30.0, 0, mp4));
  EXPECT_EQ(count_video_packets(mp4), 3);
  fs::remove_all(dir);
}

TEST(LeRobotVideo, ColorEncodeKeepsEveryFrameAtAnotherRate) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_color_rate_test");
  for (int i = 0; i < 15; ++i) write_color_frame(dir, i);
  const fs::path mp4 = dir / "color.mp4";

  // A 30 fps output rate would duplicate frames and stop at 15 packets, half a second of
  // video, so the packet count alone cannot tell the two apart.
  ASSERT_TRUE(encode_episode_video(dir, 15, 15.0, 0, mp4));
  EXPECT_EQ(count_video_packets(mp4), 15);
  EXPECT_EQ(probe_frame_rate(mp4), "15/1");
  fs::remove_all(dir);
}

TEST(LeRobotVideo, ColorEncodeRejectsAShortImageSequence) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_color_short_test");
  for (int i = 0; i < 2; ++i) write_color_frame(dir, i);

  ASSERT_TRUE(encode_episode_video(dir, 2, 30.0, 0, dir / "two.mp4"));
  EXPECT_FALSE(encode_episode_video(dir, 3, 30.0, 0, dir / "color.mp4"));
  fs::remove_all(dir);
}

TEST(LeRobotVideo, DecodeVideoRowsWritesTheFrameEachRowPlays) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_decode_rows_test");
  const fs::path annexb = dir / "color.h264";
  const std::string cmd =
    "ffmpeg -y -loglevel error -f lavfi -i testsrc=size=64x64:rate=30 -frames:v 3 "
    "-c:v libx264 -pix_fmt yuv420p -f h264 '" + annexb.string() + "'";
  ASSERT_EQ(std::system(cmd.c_str()), 0);
  auto row_image = [&dir](int row) {
    char name[32];
    std::snprintf(name, sizeof(name), kColorFramePattern, row);
    return cv::imread((dir / name).string());
  };

  // Rows 1 and 2 play the same recorded frame.
  ASSERT_TRUE(trossen::io::backends::decode_video_rows(annexb, "h264", 64, 64, {0, 1, 1, 2},
                                                       dir));
  for (int row = 0; row < 4; ++row) {
    const cv::Mat image = row_image(row);
    ASSERT_FALSE(image.empty()) << "row " << row;
    EXPECT_EQ(image.size(), cv::Size(64, 64));
  }
  EXPECT_EQ(cv::norm(row_image(1), row_image(2), cv::NORM_INF), 0.0);

  // The stream holds 3 frames, so a row that plays frame 5 cannot be written.
  EXPECT_FALSE(trossen::io::backends::decode_video_rows(annexb, "h264", 64, 64, {0, 5}, dir));
  fs::remove_all(dir);
}

TEST(LeRobotVideo, ColorEncodeReportsTheExitCode) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_color_exit_test");

  testing::internal::CaptureStderr();
  const bool ok = encode_episode_video(dir, 1, 30.0, 0, dir / "color.mp4");
  const std::string err = testing::internal::GetCapturedStderr();

  // No images, so ffmpeg exits with an error. A raw wait status would read 256 or more.
  EXPECT_FALSE(ok);
  const size_t at = err.find("(exit ");
  ASSERT_NE(at, std::string::npos) << err;
  const int code = std::atoi(err.c_str() + at + 6);
  EXPECT_GT(code, 0);
  EXPECT_LT(code, 256);
  fs::remove_all(dir);
}
