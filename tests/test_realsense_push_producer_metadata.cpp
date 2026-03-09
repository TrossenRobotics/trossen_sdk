/**
 * @file test_realsense_push_producer_metadata.cpp
 * @brief Unit tests for RealsensePushProducerMetadata
 *
 * Tests the metadata structure's get_info() and get_stream_info() methods
 * with and without depth enabled, validating the depth naming convention
 * ("_depth" suffix) that is critical for LeRobot compatibility.
 *
 * These tests do NOT require RealSense hardware — they operate on the
 * metadata struct directly.
 */

#include <string>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/hw/camera/realsense_push_producer.hpp"

using trossen::hw::camera::RealsensePushProducer;
using Metadata = RealsensePushProducer::RealsensePushProducerMetadata;

// ============================================================================
// Helper to build a metadata struct with common defaults
// ============================================================================
static Metadata make_metadata(
  const std::string& id,
  bool has_depth,
  int width = 640,
  int height = 480,
  int fps = 30)
{
  Metadata m;
  m.type = "realsense_camera";
  m.id = id;
  m.name = id;
  m.description = "test";
  m.width = width;
  m.height = height;
  m.fps = fps;
  m.codec = "av1";
  m.pix_fmt = "yuv420p";
  m.channels = 3;
  m.has_audio = false;
  m.has_depth = has_depth;
  return m;
}

// ============================================================================
// get_info() — color-only mode
// ============================================================================

TEST(RealsenseMetadataTest, GetInfoColorOnlyHasSingleFeature) {
  auto m = make_metadata("cam_wrist", /*has_depth=*/false);
  auto info = m.get_info();

  EXPECT_TRUE(info.contains("observation.images.cam_wrist"));
  EXPECT_FALSE(info.contains("observation.images.cam_wrist_depth"));
  EXPECT_EQ(info.size(), 1);
}

TEST(RealsenseMetadataTest, GetInfoColorOnlyShape) {
  auto m = make_metadata("cam_wrist", false, 640, 480);
  auto info = m.get_info();

  auto shape = info["observation.images.cam_wrist"]["shape"];
  ASSERT_EQ(shape.size(), 3);
  EXPECT_EQ(shape[0], 480);  // height
  EXPECT_EQ(shape[1], 640);  // width
  EXPECT_EQ(shape[2], 3);    // channels
}

TEST(RealsenseMetadataTest, GetInfoColorOnlyIsNotDepthMap) {
  auto m = make_metadata("cam_wrist", false);
  auto info = m.get_info();

  EXPECT_FALSE(info["observation.images.cam_wrist"]["info"]["video.is_depth_map"].get<bool>());
}

// ============================================================================
// get_info() — color + depth mode
// ============================================================================

TEST(RealsenseMetadataTest, GetInfoWithDepthHasTwoFeatures) {
  auto m = make_metadata("cam_wrist", /*has_depth=*/true);
  auto info = m.get_info();

  EXPECT_TRUE(info.contains("observation.images.cam_wrist"));
  EXPECT_TRUE(info.contains("observation.images.cam_wrist_depth"));
  EXPECT_EQ(info.size(), 2);
}

TEST(RealsenseMetadataTest, GetInfoDepthFeatureUsesUnderscoreSuffix) {
  // Critical for LeRobot compatibility: depth key must use "_depth" not "/depth"
  auto m = make_metadata("cam_overhead", true);
  auto info = m.get_info();

  EXPECT_TRUE(info.contains("observation.images.cam_overhead_depth"));
  EXPECT_FALSE(info.contains("observation.images.cam_overhead/depth"));
}

TEST(RealsenseMetadataTest, GetInfoDepthFeatureShape) {
  auto m = make_metadata("cam_wrist", true, 640, 480);
  auto info = m.get_info();

  auto shape = info["observation.images.cam_wrist_depth"]["shape"];
  ASSERT_EQ(shape.size(), 3);
  EXPECT_EQ(shape[0], 480);  // height
  EXPECT_EQ(shape[1], 640);  // width
  EXPECT_EQ(shape[2], 1);    // single-channel depth
}

TEST(RealsenseMetadataTest, GetInfoDepthFeatureIsDepthMap) {
  auto m = make_metadata("cam_wrist", true);
  auto info = m.get_info();

  EXPECT_TRUE(
    info["observation.images.cam_wrist_depth"]["info"]["video.is_depth_map"].get<bool>());
}

TEST(RealsenseMetadataTest, GetInfoDepthFeatureUsesFfv1Codec) {
  auto m = make_metadata("cam_wrist", true);
  auto info = m.get_info();

  EXPECT_EQ(
    info["observation.images.cam_wrist_depth"]["info"]["video.codec"].get<std::string>(),
    "ffv1");
}

TEST(RealsenseMetadataTest, GetInfoDepthFeatureUsesGray16lePixFmt) {
  auto m = make_metadata("cam_wrist", true);
  auto info = m.get_info();

  EXPECT_EQ(
    info["observation.images.cam_wrist_depth"]["info"]["video.pix_fmt"].get<std::string>(),
    "gray16le");
}

TEST(RealsenseMetadataTest, GetInfoDepthIdMatchesUnderscoredId) {
  auto m = make_metadata("cam_wrist", true);
  auto info = m.get_info();

  auto depth_id =
    info["observation.images.cam_wrist_depth"]["id"].get<std::string>();
  EXPECT_EQ(depth_id, "cam_wrist_depth");
}

// ============================================================================
// get_stream_info() — color-only mode
// ============================================================================

TEST(RealsenseMetadataTest, GetStreamInfoColorOnly) {
  auto m = make_metadata("cam_wrist", false, 640, 480, 30);
  auto stream = m.get_stream_info();

  ASSERT_TRUE(stream.contains("cameras"));
  EXPECT_TRUE(stream["cameras"].contains("cam_wrist"));
  EXPECT_FALSE(stream["cameras"].contains("cam_wrist_depth"));
}

TEST(RealsenseMetadataTest, GetStreamInfoColorNotDepthMap) {
  auto m = make_metadata("cam_wrist", false);
  auto stream = m.get_stream_info();

  EXPECT_FALSE(stream["cameras"]["cam_wrist"]["is_depth_map"].get<bool>());
}

// ============================================================================
// get_stream_info() — color + depth mode
// ============================================================================

TEST(RealsenseMetadataTest, GetStreamInfoWithDepth) {
  auto m = make_metadata("cam_wrist", true, 640, 480, 30);
  auto stream = m.get_stream_info();

  ASSERT_TRUE(stream["cameras"].contains("cam_wrist"));
  ASSERT_TRUE(stream["cameras"].contains("cam_wrist_depth"));
}

TEST(RealsenseMetadataTest, GetStreamInfoDepthIsDepthMap) {
  auto m = make_metadata("cam_wrist", true);
  auto stream = m.get_stream_info();

  EXPECT_TRUE(stream["cameras"]["cam_wrist_depth"]["is_depth_map"].get<bool>());
}

TEST(RealsenseMetadataTest, GetStreamInfoDepthChannel1) {
  auto m = make_metadata("cam_wrist", true);
  auto stream = m.get_stream_info();

  EXPECT_EQ(stream["cameras"]["cam_wrist_depth"]["channels"].get<int>(), 1);
}

TEST(RealsenseMetadataTest, GetStreamInfoDepthCodecFfv1) {
  auto m = make_metadata("cam_wrist", true);
  auto stream = m.get_stream_info();

  EXPECT_EQ(
    stream["cameras"]["cam_wrist_depth"]["codec"].get<std::string>(), "ffv1");
}

TEST(RealsenseMetadataTest, GetStreamInfoDimensionsMatch) {
  auto m = make_metadata("cam_wrist", true, 1280, 720, 60);
  auto stream = m.get_stream_info();

  EXPECT_EQ(stream["cameras"]["cam_wrist"]["width"].get<int>(), 1280);
  EXPECT_EQ(stream["cameras"]["cam_wrist"]["height"].get<int>(), 720);
  EXPECT_EQ(stream["cameras"]["cam_wrist"]["fps"].get<int>(), 60);

  // Depth shares the same dimensions
  EXPECT_EQ(stream["cameras"]["cam_wrist_depth"]["width"].get<int>(), 1280);
  EXPECT_EQ(stream["cameras"]["cam_wrist_depth"]["height"].get<int>(), 720);
  EXPECT_EQ(stream["cameras"]["cam_wrist_depth"]["fps"].get<int>(), 60);
}
