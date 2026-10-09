/**
 * @file test_lerobot_video.cpp
 * @brief Unit tests for the per-episode video helpers shared by the LeRobot converters.
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <unistd.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "trossen_sdk/io/backends/lerobot_common/lerobot_video.hpp"

namespace fs = std::filesystem;
using trossen::io::backends::count_video_packets;
using trossen::io::backends::encode_episode_video;
using trossen::io::backends::kColorFramePattern;
using trossen::io::backends::remux_episode_video;
using trossen::io::backends::sample_video_frames;

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

/// @brief True when file permissions bind this process; root ignores them.
bool permissions_apply() { return geteuid() != 0; }

/// @brief Encode `frames` frames of a 64x64 test pattern at 30 fps as H.264 mp4.
bool make_test_video(const fs::path& mp4, int frames) {
  const std::string cmd =
    "ffmpeg -y -loglevel error -f lavfi -i testsrc=size=64x64:rate=30 -frames:v " +
    std::to_string(frames) + " -c:v libx264 -pix_fmt yuv420p '" + mp4.string() + "'";
  return std::system(cmd.c_str()) == 0;
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

TEST(LeRobotVideo, RemuxRequiresTheExactFrameCount) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_remux_test");
  const fs::path annexb = dir / "camera.h264";
  const std::string make_stream =
    "ffmpeg -y -loglevel error -f lavfi -i testsrc=size=64x64:rate=30 -frames:v 5 "
    "-c:v libx264 -f h264 '" + annexb.string() + "'";
  ASSERT_EQ(std::system(make_stream.c_str()), 0);

  EXPECT_TRUE(remux_episode_video(annexb, 5, 30.0, dir / "match.mp4"));
  EXPECT_FALSE(remux_episode_video(annexb, 6, 30.0, dir / "more.mp4"));
  EXPECT_FALSE(remux_episode_video(annexb, 4, 30.0, dir / "fewer.mp4"));
  fs::remove_all(dir);
}

TEST(LeRobotVideo, SampleVideoFramesKeepsEveryStrideThFrame) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_sample_test");
  // Even frame n is gray level 100 + 2n and odd frames are black, so each still shows
  // which frame it came from. Lossless encoding keeps the levels exact.
  const std::string level = "if(mod(N\\,2)\\,0\\,100+2*N)";
  const std::string cmd =
    "ffmpeg -y -loglevel error -f lavfi -i \"color=black:s=64x64:r=30,format=rgb24,geq=r='" +
    level + "':g='" + level + "':b='" + level + "'\" -frames:v 60 -c:v libx264 -qp 0 "
    "-pix_fmt yuv444p '" + (dir / "camera.mp4").string() + "'";
  ASSERT_EQ(std::system(cmd.c_str()), 0);

  // 60 frames at a target of 30 samples: every second frame, starting at frame 0.
  const auto samples = sample_video_frames(dir / "camera.mp4", 0, 60, dir);
  ASSERT_EQ(samples.size(), 30u);
  for (size_t i = 0; i < samples.size(); ++i) {
    const double expected = (100.0 + 4.0 * static_cast<double>(i)) / 255.0;
    EXPECT_NEAR(cv::mean(samples[i])[0], expected, 2.0 / 255.0) << "sample " << i;
  }
  EXPECT_FALSE(fs::exists(dir / "samples"));
  fs::remove_all(dir);
}

TEST(LeRobotVideo, SampleVideoFramesSkipsFramesOutsideTheRange) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_sample_range_test");
  // Even frame n is gray level 2n and odd frames are black, so each still shows which
  // frame it came from.
  const std::string level = "if(mod(N\\,2)\\,0\\,2*N)";
  const std::string cmd =
    "ffmpeg -y -loglevel error -f lavfi -i \"color=black:s=64x64:r=30,format=rgb24,geq=r='" +
    level + "':g='" + level + "':b='" + level + "'\" -frames:v 100 -c:v libx264 -qp 0 "
    "-pix_fmt yuv444p '" + (dir / "camera.mp4").string() + "'";
  ASSERT_EQ(std::system(cmd.c_str()), 0);

  // Frames 30 to 89 at a target of 30 samples: every second frame from 30 to 88. The 30
  // frames before the range and the 10 after it are never sampled.
  const auto samples = sample_video_frames(dir / "camera.mp4", 30, 60, dir);
  ASSERT_EQ(samples.size(), 30u);
  for (size_t i = 0; i < samples.size(); ++i) {
    const double expected = (60.0 + 4.0 * static_cast<double>(i)) / 255.0;
    EXPECT_NEAR(cv::mean(samples[i])[0], expected, 2.0 / 255.0) << "sample " << i;
  }
  fs::remove_all(dir);
}

TEST(LeRobotVideo, SampleVideoFramesIgnoresStillsFromAnEarlierRun) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_sample_stale_test");
  ASSERT_TRUE(make_test_video(dir / "camera.mp4", 60));
  fs::create_directories(dir / "samples");
  ASSERT_TRUE(cv::imwrite((dir / "samples" / "sample_9999.jpg").string(),
                          cv::Mat(64, 64, CV_8UC3, cv::Scalar(0, 0, 0))));

  const auto samples = sample_video_frames(dir / "camera.mp4", 0, 60, dir);
  EXPECT_EQ(samples.size(), 30u);
  fs::remove_all(dir);
}

TEST(LeRobotVideo, SampleVideoFramesSkipsAFolderItCannotEmpty) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  if (!permissions_apply()) GTEST_SKIP() << "running as root";
  const fs::path dir = fresh_dir("trossen_lerobot_sample_locked_test");
  ASSERT_TRUE(make_test_video(dir / "camera.mp4", 60));
  ASSERT_EQ(sample_video_frames(dir / "camera.mp4", 0, 60, dir).size(), 30u);

  fs::create_directories(dir / "samples");
  ASSERT_TRUE(cv::imwrite((dir / "samples" / "sample_0001.jpg").string(),
                          cv::Mat(64, 64, CV_8UC3, cv::Scalar(0, 0, 0))));
  fs::permissions(dir / "samples", fs::perms::owner_read | fs::perms::owner_exec);
  const auto samples = sample_video_frames(dir / "camera.mp4", 0, 60, dir);
  fs::permissions(dir / "samples", fs::perms::owner_all);
  EXPECT_TRUE(samples.empty());
  fs::remove_all(dir);
}
