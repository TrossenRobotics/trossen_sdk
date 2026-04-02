/**
 * @file test_announce.cpp
 * @brief Unit tests for the announce() text-to-speech utility
 *
 * spd-say may not be installed in CI, so these tests verify graceful
 * behavior (no crash) rather than audible output.
 */

#include <string>

#include "gtest/gtest.h"

#include "trossen_sdk/utils/app_utils.hpp"

using trossen::utils::announce;

// AN-01: announce with empty string is a no-op (no crash)
TEST(AnnounceTest, EmptyString_NoOp) {
  EXPECT_NO_THROW(announce(""));
}

// AN-02: announce with blocking mode doesn't crash
TEST(AnnounceTest, BlockingMode_NoCrash) {
  EXPECT_NO_THROW(announce("test", true));
}

// AN-03: announce with non-blocking mode doesn't crash
TEST(AnnounceTest, NonBlockingMode_NoCrash) {
  EXPECT_NO_THROW(announce("test", false));
}
