/**
 * @file test_record.cpp
 * @brief Unit tests for record data structures
 */

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "opencv2/core.hpp"

#include "trossen_sdk/data/record.hpp"

using trossen::data::RecordBase;
using trossen::data::JointStateRecord;
using trossen::data::Timestamp;
using trossen::data::ImageRecord;

// Test RecordBase default construction
TEST(RecordBaseTest, DefaultConstruction) {
  RecordBase record;

  EXPECT_EQ(record.ts.monotonic.sec, 0);
  EXPECT_EQ(record.ts.monotonic.nsec, 0);
  EXPECT_EQ(record.ts.realtime.sec, 0);
  EXPECT_EQ(record.ts.realtime.nsec, 0);
  EXPECT_EQ(record.seq, 0);
  EXPECT_TRUE(record.id.empty());
}

// Test RecordBase with custom values
TEST(RecordBaseTest, CustomValues) {
  RecordBase record;
  record.ts.monotonic.sec = 100;
  record.ts.monotonic.nsec = 500'000'000;
  record.ts.realtime.sec = 1'730'000'000;
  record.ts.realtime.nsec = 250'000'000;
  record.seq = 42;
  record.id = "test_stream";

  EXPECT_EQ(record.ts.monotonic.sec, 100);
  EXPECT_EQ(record.ts.monotonic.nsec, 500'000'000);
  EXPECT_EQ(record.seq, 42);
  EXPECT_EQ(record.id, "test_stream");
}

// Test JointStateRecord default construction
TEST(JointStateRecordTest, DefaultConstruction) {
  JointStateRecord record;

  EXPECT_EQ(record.seq, 0);
  EXPECT_TRUE(record.id.empty());
  EXPECT_TRUE(record.positions.empty());
  EXPECT_TRUE(record.velocities.empty());
  EXPECT_TRUE(record.efforts.empty());
}

// Test JointStateRecord constructor with double vectors
TEST(JointStateRecordTest, ConstructorWithDoubleVectors) {
  Timestamp ts;
  ts.monotonic.sec = 10;
  ts.monotonic.nsec = 0;
  ts.realtime.sec = 1'730'000'000;
  ts.realtime.nsec = 0;

  std::vector<double> position_d = {1.0, 2.0, 3.0};
  std::vector<double> velocities_d = {0.1, 0.2, 0.3};
  std::vector<double> efforts_d = {5.0, 6.0, 7.0};

  JointStateRecord record(ts, 100, "robot_arm", position_d, velocities_d, efforts_d);

  EXPECT_EQ(record.seq, 100);
  EXPECT_EQ(record.id, "robot_arm");
  ASSERT_EQ(record.positions.size(), 3);
  ASSERT_EQ(record.velocities.size(), 3);
  ASSERT_EQ(record.efforts.size(), 3);

  // Check values (converted from double to float)
  EXPECT_FLOAT_EQ(record.positions[0], 1.0f);
  EXPECT_FLOAT_EQ(record.positions[1], 2.0f);
  EXPECT_FLOAT_EQ(record.positions[2], 3.0f);
  EXPECT_FLOAT_EQ(record.velocities[0], 0.1f);
  EXPECT_FLOAT_EQ(record.efforts[2], 7.0f);
}

// Test JointStateRecord constructor with float vectors
TEST(JointStateRecordTest, ConstructorWithFloatVectors) {
  Timestamp ts;
  ts.monotonic.sec = 20;

  std::vector<float> position_f = {1.5f, 2.5f, 3.5f, 4.5f};
  std::vector<float> velocities_f = {0.15f, 0.25f, 0.35f, 0.45f};
  std::vector<float> efforts_f = {10.0f, 20.0f, 30.0f, 40.0f};

  JointStateRecord record(ts, 200, "dual_arm", position_f, velocities_f, efforts_f);

  EXPECT_EQ(record.seq, 200);
  EXPECT_EQ(record.id, "dual_arm");
  ASSERT_EQ(record.positions.size(), 4);
  ASSERT_EQ(record.velocities.size(), 4);
  ASSERT_EQ(record.efforts.size(), 4);

  EXPECT_FLOAT_EQ(record.positions[3], 4.5f);
  EXPECT_FLOAT_EQ(record.velocities[1], 0.25f);
  EXPECT_FLOAT_EQ(record.efforts[2], 30.0f);
}

// Test JointStateRecord with empty vectors
TEST(JointStateRecordTest, EmptyVectors) {
  Timestamp ts;
  std::vector<float> empty;

  JointStateRecord record(ts, 1, "empty_robot", empty, empty, empty);

  EXPECT_EQ(record.seq, 1);
  EXPECT_TRUE(record.positions.empty());
  EXPECT_TRUE(record.velocities.empty());
  EXPECT_TRUE(record.efforts.empty());
}

// Test JointStateRecord with single joint
TEST(JointStateRecordTest, SingleJoint) {
  Timestamp ts;
  std::vector<float> position = {0.0f};
  std::vector<float> velocity = {0.5f};
  std::vector<float> effort = {15.0f};

  JointStateRecord record(ts, 5, "single_joint", position, velocity, effort);

  ASSERT_EQ(record.positions.size(), 1);
  ASSERT_EQ(record.velocities.size(), 1);
  ASSERT_EQ(record.efforts.size(), 1);

  EXPECT_FLOAT_EQ(record.positions[0], 0.0f);
  EXPECT_FLOAT_EQ(record.velocities[0], 0.5f);
  EXPECT_FLOAT_EQ(record.efforts[0], 15.0f);
}

// Test ImageRecord default construction
TEST(ImageRecordTest, DefaultConstruction) {
  ImageRecord record;

  EXPECT_EQ(record.width, 0);
  EXPECT_EQ(record.height, 0);
  EXPECT_EQ(record.channels, 0);
  EXPECT_TRUE(record.encoding.empty());
  EXPECT_TRUE(record.image.empty());
}

// Test ImageRecord with OpenCV Mat
TEST(ImageRecordTest, WithOpenCVMat) {
  ImageRecord record;
  record.width = 640;
  record.height = 480;
  record.channels = 3;
  record.encoding = "rgb8";
  record.seq = 10;
  record.id = "camera_color";

  // Create a simple OpenCV image
  record.image = cv::Mat(480, 640, CV_8UC3, cv::Scalar(100, 150, 200));

  EXPECT_EQ(record.width, 640);
  EXPECT_EQ(record.height, 480);
  EXPECT_EQ(record.channels, 3);
  EXPECT_EQ(record.encoding, "rgb8");
  EXPECT_FALSE(record.image.empty());
  EXPECT_EQ(record.image.rows, 480);
  EXPECT_EQ(record.image.cols, 640);
}

// Test ImageRecord with grayscale image
TEST(ImageRecordTest, GrayscaleImage) {
  ImageRecord record;
  record.width = 320;
  record.height = 240;
  record.channels = 1;
  record.encoding = "mono8";
  record.image = cv::Mat(240, 320, CV_8UC1, cv::Scalar(128));

  EXPECT_EQ(record.width, 320);
  EXPECT_EQ(record.height, 240);
  EXPECT_EQ(record.channels, 1);
  EXPECT_EQ(record.encoding, "mono8");
  EXPECT_EQ(record.image.type(), CV_8UC1);
}

// Test polymorphic behavior (RecordBase pointer to derived)
TEST(RecordPolymorphismTest, BasePointerToDerived) {
  auto joint_record = std::make_unique<JointStateRecord>();
  joint_record->seq = 99;
  joint_record->id = "test_joint";
  joint_record->positions = {3.0f, 4.0f};

  RecordBase* base_ptr = joint_record.get();
  EXPECT_EQ(base_ptr->seq, 99);
  EXPECT_EQ(base_ptr->id, "test_joint");

  // Downcast to verify it's still a JointStateRecord
  JointStateRecord* derived_ptr = dynamic_cast<JointStateRecord*>(base_ptr);
  ASSERT_NE(derived_ptr, nullptr);
  ASSERT_EQ(derived_ptr->positions.size(), 2);
  EXPECT_FLOAT_EQ(derived_ptr->positions[0], 3.0f);
}

// Test with realistic robot data
TEST(JointStateRecordTest, RealisticRobotData) {
  Timestamp ts = trossen::data::make_timestamp_now();

  // 6-DOF robot arm positions (radians)
  std::vector<double> positions = {0.0, -1.57, 1.57, 0.0, 1.57, 0.0};
  std::vector<double> velocities = {0.1, 0.2, -0.1, 0.05, -0.15, 0.0};
  std::vector<double> efforts = {0.5, 2.3, 1.8, 0.3, 0.9, 0.1};

  JointStateRecord record(ts, 1000, "widowxai_leader", positions, velocities, efforts);

  EXPECT_EQ(record.id, "widowxai_leader");
  EXPECT_EQ(record.seq, 1000);
  ASSERT_EQ(record.positions.size(), 6);
  EXPECT_FLOAT_EQ(record.positions[1], -1.57f);
  EXPECT_GT(record.ts.monotonic.sec, 0);
}

// ============================================================================
// ImageRecord depth field tests
// ============================================================================

// Test that depth fields are absent by default
TEST(ImageRecordTest, DefaultDepthFields) {
  ImageRecord record;

  EXPECT_FALSE(record.has_depth());
  EXPECT_FALSE(record.depth_image.has_value());
  EXPECT_FALSE(record.depth_scale.has_value());
}

// Test has_depth() after assigning a depth image
TEST(ImageRecordTest, WithDepthImage) {
  ImageRecord record;
  record.width = 640;
  record.height = 480;
  record.channels = 3;
  record.encoding = "bgr8";
  record.image = cv::Mat(480, 640, CV_8UC3, cv::Scalar(0, 128, 255));

  // Assign 16-bit depth image
  record.depth_image = cv::Mat(480, 640, CV_16UC1, cv::Scalar(1000));

  EXPECT_TRUE(record.has_depth());
  ASSERT_TRUE(record.depth_image.has_value());
  EXPECT_EQ(record.depth_image->rows, 480);
  EXPECT_EQ(record.depth_image->cols, 640);
  EXPECT_EQ(record.depth_image->type(), CV_16UC1);
  // depth_scale not set
  EXPECT_FALSE(record.depth_scale.has_value());
}

// Test both depth_image and depth_scale fields
TEST(ImageRecordTest, WithDepthImageAndScale) {
  ImageRecord record;
  record.depth_image = cv::Mat(240, 320, CV_16UC1, cv::Scalar(500));
  record.depth_scale = 0.001f;

  EXPECT_TRUE(record.has_depth());
  ASSERT_TRUE(record.depth_scale.has_value());
  EXPECT_FLOAT_EQ(record.depth_scale.value(), 0.001f);
}

// Test color image and depth image can coexist in same record
TEST(ImageRecordTest, ColorAndDepthCoexist) {
  ImageRecord record;
  record.width = 640;
  record.height = 480;

  record.image = cv::Mat(480, 640, CV_8UC3, cv::Scalar(100, 150, 200));
  record.depth_image = cv::Mat(480, 640, CV_16UC1, cv::Scalar(2000));
  record.depth_scale = 0.001f;

  EXPECT_FALSE(record.image.empty());
  EXPECT_TRUE(record.has_depth());
  EXPECT_EQ(record.image.rows, 480);
  EXPECT_EQ(record.depth_image->rows, 480);
  EXPECT_FLOAT_EQ(record.depth_scale.value(), 0.001f);
}

// Test depth_image matches expected pixel value
TEST(ImageRecordTest, DepthImagePixelValue) {
  ImageRecord record;
  const uint16_t depth_val = 1234;
  record.depth_image = cv::Mat(10, 10, CV_16UC1, cv::Scalar(depth_val));

  ASSERT_TRUE(record.depth_image.has_value());
  EXPECT_EQ(record.depth_image->at<uint16_t>(0, 0), depth_val);
  EXPECT_EQ(record.depth_image->at<uint16_t>(9, 9), depth_val);
}

// Test depth_scale boundary: zero scale
TEST(ImageRecordTest, DepthScaleZero) {
  ImageRecord record;
  record.depth_image = cv::Mat(10, 10, CV_16UC1, cv::Scalar(500));
  record.depth_scale = 0.0f;

  EXPECT_TRUE(record.has_depth());
  ASSERT_TRUE(record.depth_scale.has_value());
  EXPECT_FLOAT_EQ(record.depth_scale.value(), 0.0f);
}

// Test depth pixel at uint16 max (65535)
TEST(ImageRecordTest, DepthImageMaxUint16) {
  ImageRecord record;
  const uint16_t max_val = std::numeric_limits<uint16_t>::max();
  record.depth_image = cv::Mat(4, 4, CV_16UC1, cv::Scalar(max_val));

  ASSERT_TRUE(record.depth_image.has_value());
  EXPECT_EQ(record.depth_image->at<uint16_t>(0, 0), max_val);
  EXPECT_EQ(record.depth_image->at<uint16_t>(3, 3), max_val);
}

// Test depth_image with different dimensions than color image
TEST(ImageRecordTest, DepthAndColorDifferentDimensions) {
  ImageRecord record;
  record.width = 640;
  record.height = 480;
  record.image = cv::Mat(480, 640, CV_8UC3, cv::Scalar(0));

  // Depth at a different resolution (e.g., native depth sensor resolution)
  record.depth_image = cv::Mat(240, 320, CV_16UC1, cv::Scalar(1000));

  EXPECT_TRUE(record.has_depth());
  EXPECT_EQ(record.image.rows, 480);
  EXPECT_EQ(record.image.cols, 640);
  EXPECT_EQ(record.depth_image->rows, 240);
  EXPECT_EQ(record.depth_image->cols, 320);
}

// Test that has_depth() is false when only depth_scale is set (no image)
TEST(ImageRecordTest, DepthScaleWithoutImage) {
  ImageRecord record;
  record.depth_scale = 0.001f;

  EXPECT_FALSE(record.has_depth());
  EXPECT_FALSE(record.depth_image.has_value());
  ASSERT_TRUE(record.depth_scale.has_value());
}

// Test copy semantics preserve depth fields
TEST(ImageRecordTest, CopySemanticsWithDepth) {
  ImageRecord original;
  original.width = 640;
  original.height = 480;
  original.image = cv::Mat(480, 640, CV_8UC3, cv::Scalar(100));
  original.depth_image = cv::Mat(480, 640, CV_16UC1, cv::Scalar(2000));
  original.depth_scale = 0.001f;
  original.id = "cam_wrist";
  original.seq = 42;

  ImageRecord copy = original;

  EXPECT_TRUE(copy.has_depth());
  EXPECT_FLOAT_EQ(copy.depth_scale.value(), 0.001f);
  EXPECT_EQ(copy.depth_image->rows, 480);
  EXPECT_EQ(copy.depth_image->cols, 640);
  EXPECT_EQ(copy.id, "cam_wrist");
  EXPECT_EQ(copy.seq, 42);
  // cv::Mat uses reference counting; data pointer should be the same after shallow copy
  EXPECT_EQ(copy.depth_image->data, original.depth_image->data);
}

// Test polymorphic downcast with depth-carrying ImageRecord
TEST(ImageRecordTest, PolymorphicDowncastWithDepth) {
  auto img = std::make_shared<ImageRecord>();
  img->width = 640;
  img->height = 480;
  img->image = cv::Mat(480, 640, CV_8UC3, cv::Scalar(0));
  img->depth_image = cv::Mat(480, 640, CV_16UC1, cv::Scalar(1500));
  img->depth_scale = 0.001f;
  img->id = "cam_overhead";

  std::shared_ptr<RecordBase> base = img;

  auto* derived = dynamic_cast<ImageRecord*>(base.get());
  ASSERT_NE(derived, nullptr);
  EXPECT_TRUE(derived->has_depth());
  EXPECT_FLOAT_EQ(derived->depth_scale.value(), 0.001f);
  EXPECT_EQ(derived->id, "cam_overhead");
}
