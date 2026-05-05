/**
 * @file test_record_types.cpp
 * @brief Unit tests for Odometry2DRecord.
 *
 * Covers construction, default values, and polymorphic behavior for record
 * types not covered by the existing test_record.cpp.
 */

#include <memory>

#include "gtest/gtest.h"

#include "trossen_sdk/data/record.hpp"

using trossen::data::Odometry2DRecord;
using trossen::data::RecordBase;

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

  EXPECT_TRUE(rec.ts.monotonic.sec > 0 ||
              (rec.ts.monotonic.sec == 0 && rec.ts.monotonic.nsec > 0));
  EXPECT_EQ(rec.seq, 77);
  EXPECT_EQ(rec.id, "test_odom");
}
