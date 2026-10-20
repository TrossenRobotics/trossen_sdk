/**
 * @file test_mcap_camera_extraction.cpp
 * @brief Unit tests for extract_camera_images() on small recordings written through the
 *        Foxglove SDK, as the recorder writes them.
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <foxglove/channel.hpp>
#include <foxglove/context.hpp>
#include <foxglove/mcap.hpp>
#include <foxglove/schemas.hpp>
#include <opencv2/imgcodecs.hpp>

#include "gtest/gtest.h"
#include "mcap/reader.hpp"

#include "trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"

namespace fs = std::filesystem;

using trossen::io::backends::AlignedEpisode;
using trossen::io::backends::CameraInfo;
using trossen::io::backends::extract_camera_images;
using trossen::io::backends::McapChannelMap;

namespace {

constexpr uint64_t kFramePeriodNs = 33'333'333;

/// Serialize a Foxglove schema message.
template <typename Message>
std::vector<std::byte> encode(Message& msg) {
  std::vector<uint8_t> buffer(256);
  size_t len = 0;
  auto status = msg.encode(buffer.data(), buffer.size(), &len);
  if (status == foxglove::FoxgloveError::BufferTooShort) {
    buffer.resize(len);
    status = msg.encode(buffer.data(), buffer.size(), &len);
  }
  EXPECT_EQ(status, foxglove::FoxgloveError::Ok);
  const auto* bytes = reinterpret_cast<const std::byte*>(buffer.data());
  return std::vector<std::byte>(bytes, bytes + len);
}

/// Writes camera channels to an MCAP file with the recorder's topics and schemas.
class RecordingWriter {
 public:
  explicit RecordingWriter(const fs::path& path) : path_(path.string()) {
    foxglove::McapWriterOptions options;
    options.context = context_;
    options.path = path_;
    options.compression = foxglove::McapCompression::None;
    auto writer = foxglove::McapWriter::create(options);
    EXPECT_TRUE(writer.has_value());
    if (writer.has_value()) writer_.emplace(std::move(writer.value()));
  }

  /// Add a `foxglove.RawImage` camera with the given channel metadata.
  void add_raw_camera(const std::string& name, std::map<std::string, std::string> metadata = {}) {
    add_channel(name, foxglove::schemas::RawImage::schema(), std::move(metadata));
  }

  /// Log one RawImage frame; @p data may be shorter than the header describes.
  void log_raw(const std::string& name, uint32_t width, uint32_t height,
               const std::string& encoding, uint32_t step, std::vector<std::byte> data) {
    foxglove::schemas::RawImage msg;
    msg.width = width;
    msg.height = height;
    msg.encoding = encoding;
    msg.step = step;
    msg.data = std::move(data);
    log(name, encode(msg));
  }

  void close() {
    for (auto& [name, channel] : channels_) channel.close();
    if (writer_) EXPECT_EQ(writer_->close(), foxglove::FoxgloveError::Ok);
    writer_.reset();
  }

 private:
  void add_channel(const std::string& name, const foxglove::Schema& schema,
                   std::map<std::string, std::string> metadata) {
    auto channel = foxglove::RawChannel::create(trossen::trossen_mcap_defs::image_topic(name),
                                                "protobuf", schema, context_, std::move(metadata));
    ASSERT_TRUE(channel.has_value());
    channels_.emplace(name, std::move(channel.value()));
    log_times_[name] = 0;
  }

  void log(const std::string& name, const std::vector<std::byte>& payload) {
    const uint64_t log_time = log_times_[name];
    log_times_[name] += kFramePeriodNs;
    EXPECT_EQ(channels_.at(name).log(payload.data(), payload.size(), log_time),
              foxglove::FoxgloveError::Ok);
  }

  std::string path_;
  foxglove::Context context_ = foxglove::Context::create();
  std::optional<foxglove::McapWriter> writer_;
  std::map<std::string, foxglove::RawChannel> channels_;
  std::map<std::string, uint64_t> log_times_;
};

/// The camera channel map load_aligned_episode() builds for @p file.
McapChannelMap camera_channels(const fs::path& file) {
  McapChannelMap channels;
  std::ifstream input(file, std::ios::binary);
  mcap::McapReader reader;
  EXPECT_TRUE(reader.open(input).ok());
  EXPECT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan).ok());
  const std::string prefix = trossen::trossen_mcap_defs::kCameraTopicPrefix;
  const std::string suffix = trossen::trossen_mcap_defs::kImageTopicSuffix;
  for (const auto& [id, channel] : reader.channels()) {
    const std::string& topic = channel->topic;
    if (topic.starts_with(prefix) && topic.ends_with(suffix)) {
      channels.camera_channels[id] =
        topic.substr(prefix.size(), topic.size() - prefix.size() - suffix.size());
    }
  }
  reader.close();
  return channels;
}

/// A 2x2 bgr8 frame filled with @p value.
std::vector<std::byte> solid_bgr(uint8_t value) {
  return std::vector<std::byte>(2 * 2 * 3, static_cast<std::byte>(value));
}

/// A 2x2 16UC1 frame filled with @p raw, little-endian.
std::vector<std::byte> solid_depth(uint16_t raw) {
  std::vector<std::byte> data;
  for (int i = 0; i < 4; ++i) {
    data.push_back(static_cast<std::byte>(raw & 0xFF));
    data.push_back(static_cast<std::byte>(raw >> 8));
  }
  return data;
}

/// An episode with one camera whose rows select the given source frames.
AlignedEpisode episode_with(const std::string& camera, std::vector<size_t> row_source_index) {
  AlignedEpisode ep;
  ep.frames.resize(row_source_index.size());
  CameraInfo cam;
  cam.name = camera;
  cam.row_source_index = std::move(row_source_index);
  ep.cameras.push_back(std::move(cam));
  return ep;
}

class CameraExtractionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() /
           (std::string("trossen_extract_") + info->test_suite_name() + "_" + info->name());
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  fs::path mcap() const { return dir_ / "episode.mcap"; }

  /// Output directory for one camera, created on first use.
  fs::path out_dir(const std::string& camera) const {
    const fs::path d = dir_ / "out" / camera;
    fs::create_directories(d);
    return d;
  }

  fs::path dir_;
};

}  // namespace

TEST_F(CameraExtractionTest, WritesOneImagePerRowNumberedByRow) {
  RecordingWriter rec(mcap());
  rec.add_raw_camera("cam_high");
  for (uint8_t v : {10, 60, 110, 160}) rec.log_raw("cam_high", 2, 2, "bgr8", 6, solid_bgr(v));
  rec.close();

  // Source frame 0 is matched by two rows, frame 1 by none.
  const auto ep = episode_with("cam_high", {0, 0, 2, 3});
  std::map<std::string, size_t> counts;
  ASSERT_TRUE(extract_camera_images(mcap().string(), camera_channels(mcap()), ep,
                                    [&](const std::string& c) { return out_dir(c); }, counts));
  EXPECT_EQ(counts.at("cam_high"), 4u);

  const std::vector<int> expected = {10, 10, 110, 160};
  for (size_t row = 0; row < expected.size(); ++row) {
    char name[32];
    std::snprintf(name, sizeof(name), "image_%06zu.jpg", row);
    const cv::Mat img = cv::imread((out_dir("cam_high") / name).string(), cv::IMREAD_UNCHANGED);
    ASSERT_FALSE(img.empty()) << name;
    EXPECT_NEAR(img.at<cv::Vec3b>(0, 0)[0], expected[row], 2) << name;
  }
  EXPECT_FALSE(fs::exists(out_dir("cam_high") / "image_000004.jpg"));
}

TEST_F(CameraExtractionTest, DepthIsWrittenInMillimeters) {
  RecordingWriter rec(mcap());
  rec.add_raw_camera("cam_depth", {{"stream_type", "depth"}, {"depth_scale_m", "0.000100"}});
  rec.log_raw("cam_depth", 2, 2, "16UC1", 4, solid_depth(12346));
  rec.close();

  const auto ep = episode_with("cam_depth", {0});
  std::map<std::string, size_t> counts;
  ASSERT_TRUE(extract_camera_images(mcap().string(), camera_channels(mcap()), ep,
                                    [&](const std::string& c) { return out_dir(c); }, counts));
  const cv::Mat depth =
    cv::imread((out_dir("cam_depth") / "image_000000.png").string(), cv::IMREAD_UNCHANGED);
  ASSERT_EQ(depth.type(), CV_16UC1);
  // 12346 units of 0.1 mm is 1234.6 mm, rounded to the nearest millimeter.
  EXPECT_EQ(depth.at<uint16_t>(0, 0), 1235);
}

TEST_F(CameraExtractionTest, TruncatedImageIsRejected) {
  RecordingWriter rec(mcap());
  rec.add_raw_camera("cam_high");
  // A 2x2 bgr8 image needs 12 bytes.
  rec.log_raw("cam_high", 2, 2, "bgr8", 6, std::vector<std::byte>(7));
  rec.close();

  const auto ep = episode_with("cam_high", {0});
  std::map<std::string, size_t> counts;
  testing::internal::CaptureStderr();
  EXPECT_FALSE(extract_camera_images(mcap().string(), camera_channels(mcap()), ep,
                                     [&](const std::string& c) { return out_dir(c); }, counts));
  EXPECT_NE(testing::internal::GetCapturedStderr().find("holds 7 bytes"), std::string::npos);
}
