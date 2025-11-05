/**
 * @file test_record.cpp
 * @brief Unit tests for record data structures
 */

#include <gtest/gtest.h>
#include "trossen_sdk/data/record.hpp"
#include "opencv2/core.hpp"

using namespace trossen::data;

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
  EXPECT_TRUE(record.observations.empty());
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

  std::vector<double> action_d = {1.0, 2.0, 3.0};
  std::vector<double> observations_d = {1.0, 2.0, 3.0};
  std::vector<double> velocities_d = {0.1, 0.2, 0.3};
  std::vector<double> efforts_d = {5.0, 6.0, 7.0};

  JointStateRecord record(ts, 100, "robot_arm", action_d, observations_d, velocities_d, efforts_d);

  EXPECT_EQ(record.seq, 100);
  EXPECT_EQ(record.id, "robot_arm");
  ASSERT_EQ(record.observations.size(), 3);
  ASSERT_EQ(record.velocities.size(), 3);
  ASSERT_EQ(record.efforts.size(), 3);

  // Check values (converted from double to float)
  EXPECT_FLOAT_EQ(record.observations[0], 1.0f);
  EXPECT_FLOAT_EQ(record.observations[1], 2.0f);
  EXPECT_FLOAT_EQ(record.observations[2], 3.0f);
  EXPECT_FLOAT_EQ(record.velocities[0], 0.1f);
  EXPECT_FLOAT_EQ(record.efforts[2], 7.0f);
}

// Test JointStateRecord constructor with float vectors
TEST(JointStateRecordTest, ConstructorWithFloatVectors) {
  Timestamp ts;
  ts.monotonic.sec = 20;

  std::vector<float> action_f = {0.5f, 1.5f, 2.5f, 3.5f};
  std::vector<float> observations_f = {1.5f, 2.5f, 3.5f, 4.5f};
  std::vector<float> velocities_f = {0.15f, 0.25f, 0.35f, 0.45f};
  std::vector<float> efforts_f = {10.0f, 20.0f, 30.0f, 40.0f};

  JointStateRecord record(ts, 200, "dual_arm", action_f, observations_f, velocities_f, efforts_f);

  EXPECT_EQ(record.seq, 200);
  EXPECT_EQ(record.id, "dual_arm");
  ASSERT_EQ(record.observations.size(), 4);
  ASSERT_EQ(record.velocities.size(), 4);
  ASSERT_EQ(record.efforts.size(), 4);

  EXPECT_FLOAT_EQ(record.observations[0], 1.5f);
  EXPECT_FLOAT_EQ(record.observations[3], 4.5f);
  EXPECT_FLOAT_EQ(record.velocities[1], 0.25f);
  EXPECT_FLOAT_EQ(record.efforts[2], 30.0f);
}

// Test JointStateRecord with empty vectors
TEST(JointStateRecordTest, EmptyVectors) {
  Timestamp ts;
  std::vector<float> empty;

  JointStateRecord record(ts, 1, "empty_robot", empty, empty, empty, empty);

  EXPECT_EQ(record.seq, 1);
  EXPECT_TRUE(record.observations.empty());
  EXPECT_TRUE(record.velocities.empty());
  EXPECT_TRUE(record.efforts.empty());
}

// Test JointStateRecord with single joint
TEST(JointStateRecordTest, SingleJoint) {
  Timestamp ts;
  std::vector<float> action = {3.14159f};
  std::vector<float> obs = {0.0f};
  std::vector<float> vel = {0.5f};
  std::vector<float> eff = {15.0f};

  JointStateRecord record(ts, 5, "single_joint", action, obs, vel, eff);

  ASSERT_EQ(record.observations.size(), 1);
  ASSERT_EQ(record.velocities.size(), 1);
  ASSERT_EQ(record.efforts.size(), 1);

  EXPECT_FLOAT_EQ(record.observations[0], 3.14159f);
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
  joint_record->actions = {1.0f, 2.0f};
  joint_record->observations = {3.0f, 4.0f};

  RecordBase* base_ptr = joint_record.get();
  EXPECT_EQ(base_ptr->seq, 99);
  EXPECT_EQ(base_ptr->id, "test_joint");

  // Downcast to verify it's still a JointStateRecord
  JointStateRecord* derived_ptr = dynamic_cast<JointStateRecord*>(base_ptr);
  ASSERT_NE(derived_ptr, nullptr);
  ASSERT_EQ(derived_ptr->actions.size(), 2);
  EXPECT_FLOAT_EQ(derived_ptr->actions[0], 1.0f);
}

// Test with realistic robot data
TEST(JointStateRecordTest, RealisticRobotData) {
  Timestamp ts = make_timestamp_now();

  // 6-DOF robot arm actions (radians)
  std::vector<double> actions = {0.0, -1.57, 1.57, 0.0, 1.57, 0.0};
  std::vector<double> observations = {0.0, -1.57, 1.57, 0.0, 1.57, 0.0};
  std::vector<double> velocities = {0.1, 0.2, -0.1, 0.05, -0.15, 0.0};
  std::vector<double> efforts = {0.5, 2.3, 1.8, 0.3, 0.9, 0.1};

  JointStateRecord record(ts, 1000, "widowxai_leader", actions, observations, velocities, efforts);

  EXPECT_EQ(record.id, "widowxai_leader");
  EXPECT_EQ(record.seq, 1000);
  ASSERT_EQ(record.observations.size(), 6);
  EXPECT_FLOAT_EQ(record.observations[1], -1.57f);
  EXPECT_GT(record.ts.monotonic.sec, 0);
}
