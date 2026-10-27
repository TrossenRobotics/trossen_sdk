/**
 * @file test_mcap_dataset_loader.cpp
 * @brief Unit tests for load_aligned_episode() option validation,
 *        nearest_sample_index(), first_backward_step(), StreamWindow and
 *        row_grid().
 */

#include <cstddef>
#include <cstdint>
#include <optional>
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

TEST(LoadAlignedEpisodeTest, RejectsFpsAboveOneRowPerNanosecond) {
  EXPECT_NE(stderr_for_fps(2e9).find("one row per nanosecond"), std::string::npos);
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

namespace {

/// first_backward_step() over a vector of times.
std::optional<size_t> backward_step(const std::vector<uint64_t>& stamps) {
  return trossen::io::backends::first_backward_step(
    stamps.size(), [&](size_t i) { return stamps[i]; });
}

}  // namespace

TEST(FirstBackwardStepTest, IncreasingTimesAreInOrder) {
  EXPECT_FALSE(backward_step({0, 33, 66, 100}).has_value());
}

TEST(FirstBackwardStepTest, EqualTimesAreInOrder) {
  EXPECT_FALSE(backward_step({10, 10, 10, 20}).has_value());
}

TEST(FirstBackwardStepTest, EmptyAndSingleEntryAreInOrder) {
  EXPECT_FALSE(backward_step({}).has_value());
  EXPECT_FALSE(backward_step({5}).has_value());
}

TEST(FirstBackwardStepTest, ReportsTheFirstEntryThatStepsBack) {
  EXPECT_EQ(backward_step({0, 33, 20, 66, 10}), std::optional<size_t>{2});
}

TEST(StreamWindowTest, TheLatestStartAndEarliestEndBoundTheWindow) {
  trossen::io::backends::StreamWindow window;
  window.narrow("joints", 0, 1'000'000'000);
  window.narrow("cam_high", 320'000'000, 1'100'000'000);
  EXPECT_EQ(window.start, 320'000'000u);
  EXPECT_EQ(window.last_to_start, "cam_high");
  EXPECT_EQ(window.end, 1'000'000'000u);
  EXPECT_EQ(window.first_to_end, "joints");
  EXPECT_TRUE(window.is_open());
}

TEST(StreamWindowTest, DisjointStreamsCloseTheWindow) {
  trossen::io::backends::StreamWindow window;
  window.narrow("joints", 0, 100);
  window.narrow("cam_high", 200, 300);
  EXPECT_FALSE(window.is_open());
  EXPECT_EQ(window.first_to_end, "joints");
  EXPECT_EQ(window.last_to_start, "cam_high");
}

TEST(StreamWindowTest, ASingleSharedInstantIsNotOpen) {
  trossen::io::backends::StreamWindow window;
  window.narrow("joints", 0, 100);
  window.narrow("cam_high", 100, 200);
  EXPECT_FALSE(window.is_open());
}

TEST(RowGridTest, RowsRunAtTheDatasetRateFromTheWindowStart) {
  // A late-starting stream sets the window start, and row 0 sits on it whatever rate the
  // streams were recorded at.
  constexpr uint64_t kPeriod = 1'000'000'000 / 30;
  const auto rows = trossen::io::backends::row_grid(320'000'000, 1'320'000'000, kPeriod);
  ASSERT_EQ(rows.size(), 31u);
  EXPECT_EQ(rows.front(), 320'000'000u);
  for (size_t k = 1; k < rows.size(); ++k) EXPECT_EQ(rows[k] - rows[k - 1], kPeriod);
  EXPECT_LE(rows.back(), 1'320'000'000u);
}

TEST(RowGridTest, AnEndOnTheGridGetsARow) {
  const auto rows = trossen::io::backends::row_grid(100, 400, 100);
  EXPECT_EQ(rows, (std::vector<uint64_t>{100, 200, 300, 400}));
}
