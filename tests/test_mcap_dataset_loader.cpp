/**
 * @file test_mcap_dataset_loader.cpp
 * @brief Recording-level and unit tests for load_aligned_episode(),
 *        nearest_sample_index(), first_backward_step(), StreamWindow and
 *        row_grid().
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <foxglove/channel.hpp>
#include <foxglove/context.hpp>
#include <foxglove/mcap.hpp>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <nlohmann/json.hpp>

#include "gtest/gtest.h"

#include "JointState.pb.h"
#include "Odometry2D.pb.h"
#include "RawImage.pb.h"
#include "trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"

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

namespace fs = std::filesystem;

constexpr uint64_t kSecondNs = 1'000'000'000;
constexpr uint64_t kEpochNs = kSecondNs;
constexpr float kLeaderOffset = 10'000.0f;
constexpr float kBaseOffset = 20'000.0f;

/// Source timestamps use the exact rational rate, independently of the loader's row grid.
std::vector<uint64_t> sample_times(size_t count, uint64_t rate, uint64_t start = 0) {
  std::vector<uint64_t> times;
  for (size_t i = 0; i < count; ++i) times.push_back(start + i * kSecondNs / rate);
  return times;
}

struct RecordingTimes {
  std::vector<uint64_t> leader = sample_times(4, 30);
  std::vector<uint64_t> follower = sample_times(4, 30);
  std::vector<uint64_t> camera = sample_times(4, 30);
  // Absent means no base stream. Foxglove persists channels only when a message is logged.
  std::optional<std::vector<uint64_t>> base;
};

void append_schema(const google::protobuf::FileDescriptor* file,
                   google::protobuf::FileDescriptorSet* descriptors,
                   std::set<std::string>* visited) {
  if (!visited->insert(file->name()).second) return;
  for (int i = 0; i < file->dependency_count(); ++i) {
    append_schema(file->dependency(i), descriptors, visited);
  }
  file->CopyTo(descriptors->add_file());
}

class AlignedRecordingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Each test owns a fresh directory, including concurrent invocations of the same test.
    std::string pattern = (fs::temp_directory_path() / "trossen_loader_XXXXXX").string();
    const char* path = ::mkdtemp(pattern.data());
    ASSERT_NE(path, nullptr);
    dir_ = path;
  }

  void TearDown() override {
    if (dir_.empty()) return;
    std::error_code error;
    fs::remove_all(dir_, error);
    EXPECT_FALSE(error) << error.message();
  }

  /// Writes real protobuf messages using the recorder's schemas and topic conventions.
  /// Each joint value and base velocity identifies its source sample, not the output row.
  void write_recording(const RecordingTimes& times) {
    if (times.base) {
      ASSERT_FALSE(times.base->empty()) << "Foxglove cannot record a zero-message base channel";
    }
    const auto context = foxglove::Context::create();
    const std::string path = mcap().string();
    foxglove::McapWriterOptions options;
    options.context = context;
    options.path = path;
    options.compression = foxglove::McapCompression::None;
    auto writer = foxglove::McapWriter::create(options);
    ASSERT_TRUE(writer.has_value());
    std::map<std::string, foxglove::RawChannel> channels;

    auto add_channel = [&](const std::string& name, const std::string& topic,
                           const google::protobuf::Descriptor* descriptor) {
      google::protobuf::FileDescriptorSet descriptors;
      std::set<std::string> visited;
      append_schema(descriptor->file(), &descriptors, &visited);
      std::string bytes;
      ASSERT_TRUE(descriptors.SerializeToString(&bytes));
      foxglove::Schema schema;
      schema.name = descriptor->full_name();
      schema.encoding = "protobuf";
      schema.data = reinterpret_cast<const std::byte*>(bytes.data());
      schema.data_len = bytes.size();
      auto channel = foxglove::RawChannel::create(topic, "protobuf", schema, context, std::nullopt);
      ASSERT_TRUE(channel.has_value());
      channels.emplace(name, std::move(channel.value()));
    };
    namespace topics = trossen::trossen_mcap_defs;
    nlohmann::json dataset_info;
    dataset_info["cameras"]["cam_high"] = {
      {"width", 1}, {"height", 1}, {"is_depth_map", false}};
    if (times.base) {
      dataset_info["has_mobile_base"] = true;
      dataset_info["base_velocity_names"] = {"linear_vel", "angular_vel"};
    }
    const std::map<std::string, std::string> metadata = {
      {topics::kDatasetInfoKey, dataset_info.dump()}};
    ASSERT_EQ(writer.value().writeMetadata(topics::kRecordingMetadataName,
                                          metadata.begin(), metadata.end()),
              foxglove::FoxgloveError::Ok);
    ASSERT_NO_FATAL_FAILURE(add_channel("leader", topics::joint_state_topic("leader"),
                                       trossen_sdk::msg::JointState::descriptor()));
    ASSERT_NO_FATAL_FAILURE(add_channel("follower", topics::joint_state_topic("follower"),
                                       trossen_sdk::msg::JointState::descriptor()));
    ASSERT_NO_FATAL_FAILURE(add_channel("cam_high", topics::image_topic("cam_high"),
                                       foxglove::RawImage::descriptor()));
    if (times.base) {
      ASSERT_NO_FATAL_FAILURE(add_channel("base", topics::odometry_2d_topic("base"),
                                         trossen_sdk::msg::Odometry2D::descriptor()));
    }

    auto log = [&](const std::string& name, const google::protobuf::Message& message,
                   uint64_t time) {
      std::string bytes;
      ASSERT_TRUE(message.SerializeToString(&bytes));
      ASSERT_EQ(channels.at(name).log(reinterpret_cast<const std::byte*>(bytes.data()),
                                      bytes.size(), kEpochNs + time), foxglove::FoxgloveError::Ok);
    };
    auto log_joints = [&](const std::string& name, const std::vector<uint64_t>& stamps,
                          float offset) {
      for (size_t i = 0; i < stamps.size(); ++i) {
        trossen_sdk::msg::JointState message;
        message.set_seq(i);
        message.add_positions(offset + static_cast<float>(i));
        ASSERT_NO_FATAL_FAILURE(log(name, message, stamps[i]));
      }
    };
    ASSERT_NO_FATAL_FAILURE(log_joints("leader", times.leader, kLeaderOffset));
    ASSERT_NO_FATAL_FAILURE(log_joints("follower", times.follower, 0.0f));
    for (size_t i = 0; i < times.camera.size(); ++i) {
      foxglove::RawImage message;
      message.set_width(1);
      message.set_height(1);
      message.set_encoding("bgr8");
      message.set_step(3);
      message.set_data(std::string(3, '\0'));
      ASSERT_NO_FATAL_FAILURE(log("cam_high", message, times.camera[i]));
    }
    if (times.base) {
      for (size_t i = 0; i < times.base->size(); ++i) {
        trossen_sdk::msg::Odometry2D message;
        message.set_seq(i);
        message.mutable_twist()->set_linear_x(kBaseOffset + static_cast<float>(i));
        message.mutable_twist()->set_angular_z(-static_cast<float>(i));
        ASSERT_NO_FATAL_FAILURE(log("base", message, (*times.base)[i]));
      }
    }
    for (auto& [name, channel] : channels) channel.close();
    ASSERT_EQ(writer.value().close(), foxglove::FoxgloveError::Ok);
  }

  bool load() { return load_aligned_episode(mcap().string(), 0, episode_, channels_); }

  std::string rejection() {
    testing::internal::CaptureStderr();
    EXPECT_FALSE(load());
    return testing::internal::GetCapturedStderr();
  }

  fs::path mcap() const { return dir_ / "episode.mcap"; }

  fs::path dir_;
  AlignedEpisode episode_;
  McapChannelMap channels_;
};

}  // namespace

TEST_F(AlignedRecordingTest, Samples200HzJointsAcrossTheWhole30FpsEpisode) {
  RecordingTimes times;
  times.leader = times.follower = sample_times(2000, 200);
  times.camera = sample_times(300, 30);
  ASSERT_NO_FATAL_FAILURE(write_recording(times));
  ASSERT_TRUE(load());
  ASSERT_EQ(episode_.frames.size(), 300u);
  ASSERT_EQ(episode_.cameras.size(), 1u);
  EXPECT_EQ(episode_.cameras[0].name, "cam_high");
  ASSERT_EQ(episode_.cameras[0].row_source_index.size(), 300u);
  for (size_t row = 0; row < 300; ++row) {
    SCOPED_TRACE(row);
    const auto& frame = episode_.frames[row];
    ASSERT_EQ(frame.action.size(), 1u);
    ASSERT_EQ(frame.observation.size(), 1u);
    // At row k/30 seconds, the closest 200 Hz sample is round(k * 200 / 30).
    // This oracle uses the source rates, not nearest_sample_index() or row_grid().
    const size_t source = (row * 200 + 15) / 30;
    EXPECT_DOUBLE_EQ(frame.action[0], kLeaderOffset + source);
    EXPECT_DOUBLE_EQ(frame.observation[0], source);
    EXPECT_EQ(episode_.cameras[0].row_source_index[row], row);
    EXPECT_NEAR(frame.timestamp_s, static_cast<double>(row) / 30.0, 1e-6);
    const uint64_t ideal_time = kEpochNs + row * kSecondNs / 30;
    // A whole-nanosecond row period may round down by less than 1 ns per row.
    EXPECT_LE(frame.row_time_ns, ideal_time);
    EXPECT_LT(ideal_time - frame.row_time_ns, 300u);
  }
  // The final row must retain motion near 9.967 s, not the first 300 joint samples (1.495 s).
  EXPECT_DOUBLE_EQ(episode_.frames.back().observation[0], 1993.0);
}

TEST_F(AlignedRecordingTest, MatchesJointBaseAndCameraSamplesByTime) {
  RecordingTimes times;
  times.leader = times.follower = sample_times(6, 50);
  times.base = times.follower;
  times.camera = times.follower;
  ASSERT_NO_FATAL_FAILURE(write_recording(times));
  ASSERT_TRUE(load());
  ASSERT_EQ(episode_.frames.size(), 4u);
  ASSERT_EQ(episode_.cameras.size(), 1u);
  EXPECT_EQ(episode_.cameras[0].row_source_index, (std::vector<size_t>{0, 2, 3, 5}));
  const std::vector<size_t> expected{0, 2, 3, 5};
  for (size_t row = 0; row < expected.size(); ++row) {
    SCOPED_TRACE(row);
    const auto& frame = episode_.frames[row];
    ASSERT_EQ(frame.action.size(), 3u);
    ASSERT_EQ(frame.observation.size(), 3u);
    EXPECT_DOUBLE_EQ(frame.action[0], kLeaderOffset + expected[row]);
    EXPECT_DOUBLE_EQ(frame.observation[0], expected[row]);
    EXPECT_DOUBLE_EQ(frame.action[1], kBaseOffset + expected[row]);
    EXPECT_DOUBLE_EQ(frame.action[2], -static_cast<double>(expected[row]));
    EXPECT_DOUBLE_EQ(frame.observation[1], frame.action[1]);
    EXPECT_DOUBLE_EQ(frame.observation[2], frame.action[2]);
  }
}

TEST_F(AlignedRecordingTest, AcceptsIncreasingTimestampsInEveryStream) {
  RecordingTimes times;
  times.base = times.follower;
  ASSERT_NO_FATAL_FAILURE(write_recording(times));
  ASSERT_TRUE(load());
  EXPECT_EQ(episode_.frames.size(), 4u);
}

TEST_F(AlignedRecordingTest, AcceptsEqualAdjacentTimestampsInEveryStream) {
  RecordingTimes times;
  times.leader = times.follower = times.camera =
    {0, 33'333'333, 33'333'333, 66'666'666, 100'000'000};
  times.base = times.follower;
  ASSERT_NO_FATAL_FAILURE(write_recording(times));
  ASSERT_TRUE(load());
  EXPECT_EQ(episode_.frames.size(), 4u);
}

TEST_F(AlignedRecordingTest, RejectsBackwardJointTimestampWithStreamAndIndex) {
  RecordingTimes times;
  times.follower = {0, 66'666'666, 33'333'333, 100'000'000};
  ASSERT_NO_FATAL_FAILURE(write_recording(times));
  EXPECT_NE(rejection().find("follower timestamps go backwards at message 2"), std::string::npos);
}

TEST_F(AlignedRecordingTest, RejectsBackwardBaseTimestampWithStreamAndIndex) {
  RecordingTimes times;
  times.base = std::vector<uint64_t>{0, 66'666'666, 33'333'333, 100'000'000};
  ASSERT_NO_FATAL_FAILURE(write_recording(times));
  EXPECT_NE(rejection().find("mobile base timestamps go backwards at message 2"),
            std::string::npos);
}

TEST_F(AlignedRecordingTest, RejectsBackwardCameraTimestampWithStreamAndIndex) {
  RecordingTimes times;
  times.camera = {0, 66'666'666, 33'333'333, 100'000'000};
  ASSERT_NO_FATAL_FAILURE(write_recording(times));
  EXPECT_NE(rejection().find("cam_high timestamps go backwards at message 2"), std::string::npos);
}

TEST_F(AlignedRecordingTest, LateCameraSetsTheFirstRowTime) {
  RecordingTimes times;
  times.leader = times.follower = sample_times(31, 200);
  times.camera = sample_times(4, 30, 20'000'000);
  ASSERT_NO_FATAL_FAILURE(write_recording(times));
  ASSERT_TRUE(load());
  ASSERT_EQ(episode_.frames.size(), 4u);
  EXPECT_EQ(episode_.frames.front().row_time_ns, kEpochNs + 20'000'000);
  EXPECT_FLOAT_EQ(episode_.frames.front().timestamp_s, 0.0f);
  ASSERT_EQ(episode_.frames.front().observation.size(), 1u);
  EXPECT_DOUBLE_EQ(episode_.frames.front().observation[0], 4.0);
}

TEST_F(AlignedRecordingTest, LateMobileBaseSetsTheFirstRowTime) {
  RecordingTimes times;
  times.leader = times.follower = sample_times(31, 200);
  times.camera = sample_times(5, 30);
  times.base = sample_times(4, 30, 20'000'000);
  ASSERT_NO_FATAL_FAILURE(write_recording(times));
  ASSERT_TRUE(load());
  ASSERT_EQ(episode_.frames.size(), 4u);
  EXPECT_EQ(episode_.frames.front().row_time_ns, kEpochNs + 20'000'000);
  EXPECT_FLOAT_EQ(episode_.frames.front().timestamp_s, 0.0f);
}

TEST_F(AlignedRecordingTest, RejectsDisjointStreamWindows) {
  RecordingTimes times;
  times.camera = sample_times(4, 30, 200'000'000);
  ASSERT_NO_FATAL_FAILURE(write_recording(times));
  const std::string error = rejection();
  EXPECT_NE(error.find("no stretch of time has data from every stream"), std::string::npos);
  EXPECT_NE(error.find("cam_high"), std::string::npos);
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
