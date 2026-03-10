/**
 * @file test_record_types.cpp
 * @brief Unit tests for TeleopJointStateRecord and Odometry2DRecord
 *
 * Covers construction, type conversion, default values, and polymorphic
 * behavior for record types not covered by the existing test_record.cpp.
 */

#include <cstdint>
#include <memory>
#include <vector>

#include "gtest/gtest.h"

#include "trossen_sdk/data/record.hpp"

using trossen::data::Odometry2DRecord;
using trossen::data::RecordBase;
using trossen::data::TeleopJointStateRecord;
using trossen::data::Timestamp;

// ============================================================================
// TeleopJointStateRecord tests
// ============================================================================

// REC-01: Double vector constructor converts correctly
TEST(TeleopJointStateRecordTest, DoubleConstructor) {
  Timestamp ts;
  ts.monotonic.sec = 10;
  ts.realtime.sec = 1'730'000'000;

  std::vector<double> actions_d = {1.0, 2.0, 3.0, 4.0};
  std::vector<double> observations_d = {0.5, 1.5, 2.5, 3.5};

  TeleopJointStateRecord rec(ts, 42, "teleop/joints", actions_d, observations_d);

  EXPECT_EQ(rec.seq, 42);
  EXPECT_EQ(rec.id, "teleop/joints");

  ASSERT_EQ(rec.actions.size(), 4);
  ASSERT_EQ(rec.observations.size(), 4);

  // Verify double-to-float conversion
  EXPECT_FLOAT_EQ(rec.actions[0], 1.0f);
  EXPECT_FLOAT_EQ(rec.actions[3], 4.0f);
  EXPECT_FLOAT_EQ(rec.observations[0], 0.5f);
  EXPECT_FLOAT_EQ(rec.observations[3], 3.5f);
}

// REC-02: Float vector constructor preserves values
TEST(TeleopJointStateRecordTest, FloatConstructor) {
  Timestamp ts;
  std::vector<float> actions_f = {1.5f, 2.5f};
  std::vector<float> observations_f = {3.5f, 4.5f};

  TeleopJointStateRecord rec(ts, 10, "teleop/arm", actions_f, observations_f);

  ASSERT_EQ(rec.actions.size(), 2);
  ASSERT_EQ(rec.observations.size(), 2);

  EXPECT_FLOAT_EQ(rec.actions[0], 1.5f);
  EXPECT_FLOAT_EQ(rec.actions[1], 2.5f);
  EXPECT_FLOAT_EQ(rec.observations[0], 3.5f);
  EXPECT_FLOAT_EQ(rec.observations[1], 4.5f);
}

// Default construction
TEST(TeleopJointStateRecordTest, DefaultConstruction) {
  TeleopJointStateRecord rec;

  EXPECT_EQ(rec.seq, 0);
  EXPECT_TRUE(rec.id.empty());
  EXPECT_TRUE(rec.actions.empty());
  EXPECT_TRUE(rec.observations.empty());
}

// Empty vectors
TEST(TeleopJointStateRecordTest, EmptyVectors) {
  Timestamp ts;
  std::vector<float> empty;

  TeleopJointStateRecord rec(ts, 1, "empty", empty, empty);

  EXPECT_TRUE(rec.actions.empty());
  EXPECT_TRUE(rec.observations.empty());
}

// Polymorphic downcast
TEST(TeleopJointStateRecordTest, PolymorphicDowncast) {
  auto rec = std::make_unique<TeleopJointStateRecord>();
  rec->seq = 99;
  rec->id = "teleop";
  rec->actions = {1.0f, 2.0f};
  rec->observations = {3.0f, 4.0f};

  RecordBase* base = rec.get();
  EXPECT_EQ(base->seq, 99);

  auto* derived = dynamic_cast<TeleopJointStateRecord*>(base);
  ASSERT_NE(derived, nullptr);
  EXPECT_EQ(derived->actions.size(), 2);
}

// ============================================================================
// Odometry2DRecord tests
// ============================================================================

// REC-03: Default values are zero
TEST(Odometry2DRecordTest, DefaultValues) {
  Odometry2DRecord rec;

  EXPECT_FLOAT_EQ(rec.pose.x, 0.0f);
  EXPECT_FLOAT_EQ(rec.pose.y, 0.0f);
  EXPECT_FLOAT_EQ(rec.pose.theta, 0.0f);
  EXPECT_FLOAT_EQ(rec.twist.linear_x, 0.0f);
  EXPECT_FLOAT_EQ(rec.twist.linear_y, 0.0f);
  EXPECT_FLOAT_EQ(rec.twist.angular_z, 0.0f);
}

// Custom values
TEST(Odometry2DRecordTest, CustomValues) {
  Odometry2DRecord rec;
  rec.pose.x = 1.5f;
  rec.pose.y = 2.5f;
  rec.pose.theta = 0.785f;  // ~45 degrees
  rec.twist.linear_x = 0.5f;
  rec.twist.angular_z = 0.1f;
  rec.id = "base/odom";
  rec.seq = 100;

  EXPECT_FLOAT_EQ(rec.pose.x, 1.5f);
  EXPECT_FLOAT_EQ(rec.twist.linear_x, 0.5f);
  EXPECT_EQ(rec.id, "base/odom");
}

// REC-04: Polymorphic downcast
TEST(Odometry2DRecordTest, PolymorphicDowncast) {
  auto rec = std::make_shared<Odometry2DRecord>();
  rec->seq = 50;
  rec->id = "odom";
  rec->pose.x = 1.0f;

  std::shared_ptr<RecordBase> base = rec;

  auto* derived = dynamic_cast<Odometry2DRecord*>(base.get());
  ASSERT_NE(derived, nullptr);
  EXPECT_FLOAT_EQ(derived->pose.x, 1.0f);
  EXPECT_EQ(derived->id, "odom");
}

// Ensure RecordBase fields inherited correctly
TEST(Odometry2DRecordTest, InheritedFields) {
  Odometry2DRecord rec;
  rec.ts = trossen::data::make_timestamp_now();
  rec.seq = 77;
  rec.id = "test_odom";

  EXPECT_GT(rec.ts.monotonic.sec, 0);
  EXPECT_EQ(rec.seq, 77);
  EXPECT_EQ(rec.id, "test_odom");
}
