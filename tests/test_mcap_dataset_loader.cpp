/**
 * @file test_mcap_dataset_loader.cpp
 * @brief Unit tests for load_aligned_episode() option validation.
 */

#include <string>

#include "gtest/gtest.h"

#include "trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp"

using trossen::io::backends::AlignedEpisode;
using trossen::io::backends::AlignmentOptions;
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
