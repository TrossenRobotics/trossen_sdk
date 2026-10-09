/**
 * @file test_mcap_dataset_loader.cpp
 * @brief Unit tests for load_aligned_episode() option validation and
 *        nearest_sample_index().
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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

namespace {

using trossen::io::backends::NO_SAMPLE;

/// Nearest entry of `stamps` to `target` within `tolerance_ns`, starting at `cursor`.
size_t nearest(
  const std::vector<uint64_t>& stamps, uint64_t target, uint64_t tolerance_ns, size_t& cursor)
{
  return trossen::io::backends::nearest_sample_index(
    stamps.size(), [&](size_t i) { return stamps[i]; }, target, tolerance_ns, cursor);
}

}  // namespace

TEST(NearestSampleIndexTest, PicksTheCloserLaterSample) {
  const std::vector<uint64_t> stamps{0, 40};
  size_t cursor = 0;
  EXPECT_EQ(nearest(stamps, 30, 100, cursor), 1u);
  EXPECT_EQ(cursor, 0u);
}

TEST(NearestSampleIndexTest, PicksTheCloserEarlierSample) {
  const std::vector<uint64_t> stamps{0, 40};
  size_t cursor = 0;
  EXPECT_EQ(nearest(stamps, 10, 100, cursor), 0u);
}

TEST(NearestSampleIndexTest, EqualDistanceKeepsTheEarlierSample) {
  const std::vector<uint64_t> stamps{0, 40};
  size_t cursor = 0;
  EXPECT_EQ(nearest(stamps, 20, 100, cursor), 0u);
}

TEST(NearestSampleIndexTest, ToleranceIsInclusive) {
  const std::vector<uint64_t> stamps{100};
  size_t cursor = 0;
  EXPECT_EQ(nearest(stamps, 110, 10, cursor), 0u);
  EXPECT_EQ(nearest(stamps, 111, 10, cursor), NO_SAMPLE);
  EXPECT_EQ(nearest(stamps, 90, 10, cursor), 0u);
  EXPECT_EQ(nearest(stamps, 89, 10, cursor), NO_SAMPLE);
}

TEST(NearestSampleIndexTest, EmptyInputHasNoSample) {
  const std::vector<uint64_t> stamps;
  size_t cursor = 0;
  EXPECT_EQ(nearest(stamps, 0, 100, cursor), NO_SAMPLE);
}

TEST(NearestSampleIndexTest, SuccessiveTargetsShareTheCursor) {
  const std::vector<uint64_t> stamps{0, 33, 66, 100, 133};
  size_t cursor = 0;
  EXPECT_EQ(nearest(stamps, 30, 20, cursor), 1u);
  EXPECT_EQ(cursor, 0u);
  EXPECT_EQ(nearest(stamps, 70, 20, cursor), 2u);
  EXPECT_EQ(cursor, 2u);
  EXPECT_EQ(nearest(stamps, 140, 20, cursor), 4u);
  EXPECT_EQ(cursor, 4u);
  // The cursor never moves back, so an earlier target is compared with entry 4 only.
  EXPECT_EQ(nearest(stamps, 0, 20, cursor), NO_SAMPLE);
}
