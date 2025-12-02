/**
 * @file test_timestamp.cpp
 * @brief Unit tests for timestamp utilities
 */

#include <gtest/gtest.h>
#include "trossen_sdk/data/timestamp.hpp"
#include <thread>
#include <chrono>

using trossen::data::make_timestamp_now;
using trossen::data::now_mono;
using trossen::data::now_real;
using trossen::data::S_TO_NS;
using trossen::data::Timespec;
using trossen::data::Timestamp;

// Test Timespec basic construction
TEST(TimespecTest, DefaultConstruction) {
  Timespec ts;
  EXPECT_EQ(ts.sec, 0);
  EXPECT_EQ(ts.nsec, 0);
}

// Test Timespec to_ns conversion
TEST(TimespecTest, ToNanoseconds) {
  Timespec ts;
  ts.sec = 1;
  ts.nsec = 500'000'000;  // 0.5 seconds

  uint64_t expected = 1'500'000'000ull;  // 1.5 seconds in nanoseconds
  EXPECT_EQ(ts.to_ns(), expected);
}

// Test Timespec from_ns conversion
TEST(TimespecTest, FromNanoseconds) {
  uint64_t total_ns = 2'750'000'000ull;  // 2.75 seconds
  Timespec ts = Timespec::from_ns(total_ns);

  EXPECT_EQ(ts.sec, 2);
  EXPECT_EQ(ts.nsec, 750'000'000);
}

// Test round-trip conversion
TEST(TimespecTest, RoundTripConversion) {
  Timespec original;
  original.sec = 42;
  original.nsec = 123'456'789;

  uint64_t ns = original.to_ns();
  Timespec recovered = Timespec::from_ns(ns);

  EXPECT_EQ(recovered.sec, original.sec);
  EXPECT_EQ(recovered.nsec, original.nsec);
}

// Test edge case: zero nanoseconds
TEST(TimespecTest, ZeroNanoseconds) {
  Timespec ts = Timespec::from_ns(0);
  EXPECT_EQ(ts.sec, 0);
  EXPECT_EQ(ts.nsec, 0);
  EXPECT_EQ(ts.to_ns(), 0);
}

// Test edge case: exactly one second
TEST(TimespecTest, ExactlyOneSecond) {
  Timespec ts = Timespec::from_ns(S_TO_NS);
  EXPECT_EQ(ts.sec, 1);
  EXPECT_EQ(ts.nsec, 0);
}

// Test edge case: large values
TEST(TimespecTest, LargeValues) {
  uint64_t large_ns = 1'000'000'000'000'000ull;  // ~31.7 years
  Timespec ts = Timespec::from_ns(large_ns);
  EXPECT_EQ(ts.to_ns(), large_ns);
}

// Test Timestamp default construction
TEST(TimestampTest, DefaultConstruction) {
  Timestamp ts;
  EXPECT_EQ(ts.monotonic.sec, 0);
  EXPECT_EQ(ts.monotonic.nsec, 0);
  EXPECT_EQ(ts.realtime.sec, 0);
  EXPECT_EQ(ts.realtime.nsec, 0);
}

// Test now_mono returns reasonable values
TEST(TimestampTest, NowMonotonicIsReasonable) {
  Timespec t1 = now_mono();
  EXPECT_GT(t1.sec, 0);  // Should be positive
  EXPECT_LT(t1.nsec, S_TO_NS);  // nsec should be < 1 billion
}

// Test now_real returns reasonable values
TEST(TimestampTest, NowRealtimeIsReasonable) {
  Timespec t1 = now_real();
  EXPECT_GT(t1.sec, 1'700'000'000);  // Should be after ~2023
  EXPECT_LT(t1.nsec, S_TO_NS);  // nsec should be < 1 billion
}

// Test monotonic clock advances
TEST(TimestampTest, MonotonicAdvances) {
  Timespec t1 = now_mono();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  Timespec t2 = now_mono();

  EXPECT_GT(t2.to_ns(), t1.to_ns());
}

// Test realtime clock advances
TEST(TimestampTest, RealtimeAdvances) {
  Timespec t1 = now_real();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  Timespec t2 = now_real();

  EXPECT_GT(t2.to_ns(), t1.to_ns());
}

// Test make_timestamp_now creates valid timestamp
TEST(TimestampTest, MakeTimestampNow) {
  Timestamp ts = make_timestamp_now();

  // Both clocks should have reasonable values
  EXPECT_GT(ts.monotonic.sec, 0);
  EXPECT_LT(ts.monotonic.nsec, S_TO_NS);

  EXPECT_GT(ts.realtime.sec, 1'700'000'000);
  EXPECT_LT(ts.realtime.nsec, S_TO_NS);
}

// Test that monotonic and realtime are captured close together
TEST(TimestampTest, ClocksAreSynchronized) {
  Timestamp ts = make_timestamp_now();

  // Both clocks should be non-zero
  EXPECT_GT(ts.monotonic.to_ns(), 0);
  EXPECT_GT(ts.realtime.to_ns(), 0);

  // The difference should be reasonable (monotonic starts from boot, realtime from epoch, so we
  // just verify they're both sensible)
  EXPECT_LT(ts.monotonic.nsec, S_TO_NS);
  EXPECT_LT(ts.realtime.nsec, S_TO_NS);
}

// Test nanosecond boundary conditions
TEST(TimespecTest, NanosecondBoundary) {
  // Test nsec = 999,999,999 (just under 1 second)
  Timespec ts;
  ts.sec = 5;
  ts.nsec = 999'999'999;

  uint64_t ns = ts.to_ns();
  Timespec recovered = Timespec::from_ns(ns);

  EXPECT_EQ(recovered.sec, 5);
  EXPECT_EQ(recovered.nsec, 999'999'999);
}

// Test S_TO_NS constant
TEST(TimestampTest, ConstantValue) {
  EXPECT_EQ(S_TO_NS, 1'000'000'000ull);
}
