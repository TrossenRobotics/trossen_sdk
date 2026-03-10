/**
 * @file test_lerobot_utils.cpp
 * @brief Unit tests for LeRobotV2 inline utility functions
 *
 * Tests filename formatting, metadata JSON helpers, and naming conventions
 * for episodes, chunks, videos, and standard metadata features.
 */

#include <string>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_backend.hpp"

using trossen::io::backends::format_chunk_dir;
using trossen::io::backends::format_episode_folder;
using trossen::io::backends::format_episode_parquet;
using trossen::io::backends::format_image_filename;
using trossen::io::backends::format_video_filename;
using trossen::io::backends::create_scalar_feature;
using trossen::io::backends::add_standard_metadata_features;

// ============================================================================
// LR-01: format_episode_folder zero-padded
// ============================================================================

TEST(LeRobotUtilsTest, FormatEpisodeFolder_Zero) {
  EXPECT_EQ(format_episode_folder(0), "episode_000000");
}

TEST(LeRobotUtilsTest, FormatEpisodeFolder_NonZero) {
  EXPECT_EQ(format_episode_folder(1), "episode_000001");
  EXPECT_EQ(format_episode_folder(42), "episode_000042");
  EXPECT_EQ(format_episode_folder(999999), "episode_999999");
}

// ============================================================================
// LR-02: format_chunk_dir zero-padded
// ============================================================================

TEST(LeRobotUtilsTest, FormatChunkDir_Zero) {
  EXPECT_EQ(format_chunk_dir(0), "chunk-000");
}

TEST(LeRobotUtilsTest, FormatChunkDir_NonZero) {
  EXPECT_EQ(format_chunk_dir(1), "chunk-001");
  EXPECT_EQ(format_chunk_dir(42), "chunk-042");
  EXPECT_EQ(format_chunk_dir(999), "chunk-999");
}

// ============================================================================
// LR-03: format_episode_parquet has correct extension
// ============================================================================

TEST(LeRobotUtilsTest, FormatEpisodeParquet_Extension) {
  std::string name = format_episode_parquet(0);
  EXPECT_EQ(name, "episode_000000.parquet");

  std::string ext = name.substr(name.rfind('.'));
  EXPECT_EQ(ext, ".parquet");
}

TEST(LeRobotUtilsTest, FormatEpisodeParquet_Index) {
  EXPECT_EQ(format_episode_parquet(5), "episode_000005.parquet");
}

// ============================================================================
// LR-04: format_video_filename has .mp4 extension
// ============================================================================

TEST(LeRobotUtilsTest, FormatVideoFilename_Extension) {
  std::string name = format_video_filename(0);
  EXPECT_EQ(name, "episode_000000.mp4");

  std::string ext = name.substr(name.rfind('.'));
  EXPECT_EQ(ext, ".mp4");
}

TEST(LeRobotUtilsTest, FormatVideoFilename_Index) {
  EXPECT_EQ(format_video_filename(99), "episode_000099.mp4");
}

// ============================================================================
// Image filename formatting (color = jpg)
// ============================================================================

TEST(LeRobotUtilsTest, FormatImageFilename_Extension) {
  std::string name = format_image_filename(0);
  EXPECT_EQ(name, "image_000000.jpg");

  std::string ext = name.substr(name.rfind('.'));
  EXPECT_EQ(ext, ".jpg");
}

// ============================================================================
// LR-05: create_scalar_feature shape
// ============================================================================

TEST(LeRobotUtilsTest, CreateScalarFeature_Shape) {
  auto feature = create_scalar_feature("float32");

  EXPECT_EQ(feature["dtype"], "float32");
  ASSERT_TRUE(feature["shape"].is_array());
  ASSERT_EQ(feature["shape"].size(), 1);
  EXPECT_EQ(feature["shape"][0], 1);
  EXPECT_TRUE(feature["names"].is_array());
  EXPECT_TRUE(feature["names"].empty());
}

TEST(LeRobotUtilsTest, CreateScalarFeature_Int64) {
  auto feature = create_scalar_feature("int64");
  EXPECT_EQ(feature["dtype"], "int64");
}

// ============================================================================
// LR-06: add_standard_metadata_features all present
// ============================================================================

TEST(LeRobotUtilsTest, AddStandardMetadataFeatures_AllPresent) {
  nlohmann::ordered_json features;
  add_standard_metadata_features(features);

  EXPECT_TRUE(features.contains("timestamp"));
  EXPECT_TRUE(features.contains("frame_index"));
  EXPECT_TRUE(features.contains("episode_index"));
  EXPECT_TRUE(features.contains("index"));
  EXPECT_TRUE(features.contains("task_index"));

  // Each should have dtype, shape, names
  EXPECT_EQ(features["timestamp"]["dtype"], "float32");
  EXPECT_EQ(features["frame_index"]["dtype"], "int64");
  EXPECT_EQ(features["episode_index"]["dtype"], "int64");
  EXPECT_EQ(features["index"]["dtype"], "int64");
  EXPECT_EQ(features["task_index"]["dtype"], "int64");
}

// Verify standard features don't overwrite existing features
TEST(LeRobotUtilsTest, AddStandardMetadataFeatures_DoesNotOverwriteExisting) {
  nlohmann::ordered_json features;
  features["custom_field"] = {{"dtype", "bool"}};

  add_standard_metadata_features(features);

  // Custom field should still exist
  EXPECT_TRUE(features.contains("custom_field"));
  EXPECT_EQ(features["custom_field"]["dtype"], "bool");

  // Standard features should also be present
  EXPECT_TRUE(features.contains("timestamp"));
}
