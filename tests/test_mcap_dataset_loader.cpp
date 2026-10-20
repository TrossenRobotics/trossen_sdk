/**
 * @file test_mcap_dataset_loader.cpp
 * @brief Unit tests for load_aligned_episode() option validation and
 *        clamp_episode_to_video_frame_counts().
 */

#include <map>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp"

using trossen::io::backends::AlignedEpisode;
using trossen::io::backends::AlignmentOptions;
using trossen::io::backends::CameraInfo;
using trossen::io::backends::CameraVideoStream;
using trossen::io::backends::clamp_episode_to_video_frame_counts;
using trossen::io::backends::load_aligned_episode;
using trossen::io::backends::McapChannelMap;

namespace {

/// Runs the loader with the given rate and returns what it printed to stderr. The rate is
/// checked before the file is opened, so no recording is needed.
std::string stderr_for_fps(double fps) {
  AlignedEpisode episode;
  McapChannelMap channels;
  AlignmentOptions alignment;
  alignment.fps = fps;
  testing::internal::CaptureStderr();
  EXPECT_FALSE(load_aligned_episode("/nonexistent.mcap", 0, episode, channels, {}, alignment));
  return testing::internal::GetCapturedStderr();
}

}  // namespace

TEST(LoadAlignedEpisodeTest, RejectsFractionalFps) {
  EXPECT_NE(stderr_for_fps(29.97).find("whole number"), std::string::npos);
}

TEST(LoadAlignedEpisodeTest, RejectsNonPositiveFps) {
  EXPECT_NE(stderr_for_fps(0.0).find("whole number"), std::string::npos);
  EXPECT_NE(stderr_for_fps(-30.0).find("whole number"), std::string::npos);
}

TEST(LoadAlignedEpisodeTest, AcceptsWholeFpsAndFailsOnTheMissingFile) {
  EXPECT_EQ(stderr_for_fps(30.0).find("whole number"), std::string::npos);
}

namespace {

/// An episode of @p rows rows; each camera selects consecutive frames from its start frame.
AlignedEpisode episode_with_cameras(size_t rows, const std::map<std::string, size_t>& first_frame) {
  AlignedEpisode ep;
  ep.frames.resize(rows);
  for (const auto& [name, first] : first_frame) {
    CameraInfo cam;
    cam.name = name;
    for (size_t row = 0; row < rows; ++row) cam.row_source_index.push_back(first + row);
    ep.cameras.push_back(cam);
  }
  return ep;
}

/// A video stream holding @p frames frames.
CameraVideoStream stream_of(size_t frames) {
  CameraVideoStream stream;
  stream.frame_count = frames;
  return stream;
}

}  // namespace

TEST(ClampToVideoFrameCountsTest, ShortestVideoWins) {
  // cam_depth has no video stream, as a raw-image camera would not.
  auto ep = episode_with_cameras(10, {{"cam_high", 0}, {"cam_low", 0}, {"cam_depth", 0}});
  clamp_episode_to_video_frame_counts(ep, {{"cam_high", stream_of(8)}, {"cam_low", stream_of(6)}},
                                      /*start_offset_applied=*/false);

  ASSERT_EQ(ep.frames.size(), 6u);
  for (const auto& cam : ep.cameras) {
    EXPECT_EQ(cam.row_source_index, std::vector<size_t>({0, 1, 2, 3, 4, 5})) << cam.name;
  }
}

TEST(ClampToVideoFrameCountsTest, FramesBeforeTheStartOffsetDoNotCount) {
  // Row 0 plays frame 3, so a 10-frame video covers 7 rows once its start is moved.
  auto ep = episode_with_cameras(10, {{"cam_high", 3}});
  clamp_episode_to_video_frame_counts(ep, {{"cam_high", stream_of(10)}},
                                      /*start_offset_applied=*/true);
  ASSERT_EQ(ep.frames.size(), 7u);
  EXPECT_EQ(ep.cameras[0].row_source_index.front(), 3u);
  EXPECT_EQ(ep.cameras[0].row_source_index.back(), 9u);

  // Without the moved start, the same video covers all 10 rows.
  auto unshifted = episode_with_cameras(10, {{"cam_high", 3}});
  clamp_episode_to_video_frame_counts(unshifted, {{"cam_high", stream_of(10)}},
                                      /*start_offset_applied=*/false);
  EXPECT_EQ(unshifted.frames.size(), 10u);
}

TEST(ClampToVideoFrameCountsTest, LeavesTheEpisodeAloneWhenEveryVideoCoversIt) {
  auto ep = episode_with_cameras(5, {{"cam_high", 0}});
  clamp_episode_to_video_frame_counts(ep, {}, false);
  EXPECT_EQ(ep.frames.size(), 5u);

  clamp_episode_to_video_frame_counts(ep, {{"cam_high", stream_of(12)}}, false);
  EXPECT_EQ(ep.frames.size(), 5u);

  // A stream with no frames was never extracted, so it does not shorten the episode.
  clamp_episode_to_video_frame_counts(ep, {{"cam_high", stream_of(0)}}, false);
  EXPECT_EQ(ep.frames.size(), 5u);
  EXPECT_EQ(ep.cameras[0].row_source_index.size(), 5u);
}
