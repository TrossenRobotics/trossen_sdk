/**
 * @file test_lerobot_depth_utils.cpp
 * @brief Unit tests for LeRobot V2 depth utility functions
 *
 * Tests the format_depth_filename() helper and validates the depth naming
 * conventions used by the LeRobot V2 backend for depth image storage.
 */

#include <string>

#include "gtest/gtest.h"

#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_backend.hpp"

using trossen::io::backends::format_depth_filename;
using trossen::io::backends::format_image_filename;

// ============================================================================
// format_depth_filename() tests
// ============================================================================

TEST(LeRobotDepthUtilsTest, DepthFilenameFrame0) {
  EXPECT_EQ(format_depth_filename(0), "image_000000.png");
}

TEST(LeRobotDepthUtilsTest, DepthFilenameFrame1) {
  EXPECT_EQ(format_depth_filename(1), "image_000001.png");
}

TEST(LeRobotDepthUtilsTest, DepthFilenameFrame999999) {
  EXPECT_EQ(format_depth_filename(999999), "image_999999.png");
}

TEST(LeRobotDepthUtilsTest, DepthFilenameFrame42) {
  EXPECT_EQ(format_depth_filename(42), "image_000042.png");
}

// Depth uses PNG (lossless 16-bit), not JPEG
TEST(LeRobotDepthUtilsTest, DepthFilenameExtensionIsPng) {
  std::string name = format_depth_filename(0);
  EXPECT_GT(name.size(), 4u);
  EXPECT_EQ(name.substr(name.size() - 4), ".png");
}

// ============================================================================
// format_image_filename() vs format_depth_filename() consistency
// ============================================================================

// Color and depth filenames should differ in extension (jpg vs png)
TEST(LeRobotDepthUtilsTest, ColorAndDepthExtensionsDiffer) {
  std::string color = format_image_filename(0);
  std::string depth = format_depth_filename(0);

  // Both should have the same frame index prefix
  EXPECT_EQ(color.substr(0, color.rfind('.')), depth.substr(0, depth.rfind('.')));

  // Extensions should differ (color=jpg, depth=png for lossless 16-bit)
  std::string color_ext = color.substr(color.rfind('.'));
  std::string depth_ext = depth.substr(depth.rfind('.'));
  EXPECT_NE(color_ext, depth_ext);
}

// ============================================================================
// Depth naming convention: underscore-delimited IDs
// ============================================================================

// This is a convention test, not a function test — validates the design decision
// that depth stream IDs use "_depth" suffix, not "/depth"
TEST(LeRobotDepthUtilsTest, DepthIdConventionUsesUnderscore) {
  // Simulate what the backend does (from lerobot_v2_backend.cpp):
  // const std::string depth_key = img.id + "_depth";
  const std::string cam_id = "cam_wrist";
  const std::string depth_key = cam_id + "_depth";

  EXPECT_EQ(depth_key, "cam_wrist_depth");

  // Must NOT use path-style separator (breaks LeRobot flatten_dict)
  EXPECT_EQ(depth_key.find('/'), std::string::npos);
}
