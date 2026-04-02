/**
 * @file test_announce.cpp
 * @brief Unit tests for the announce() text-to-speech utility
 *
 * Tests verify graceful behavior (no crash) without producing audio.
 * PATH is temporarily cleared so posix_spawnp cannot find spd-say.
 */

#include <cstdlib>
#include <string>

#include "gtest/gtest.h"

#include "trossen_sdk/utils/app_utils.hpp"

using trossen::utils::announce;

// RAII guard that hides spd-say by clearing PATH for the duration of the test
class SilentAnnounceTest : public ::testing::Test {
protected:
  void SetUp() override {
    const char* p = std::getenv("PATH");
    saved_path_ = p ? p : "";
    setenv("PATH", "", 1);
  }
  void TearDown() override {
    setenv("PATH", saved_path_.c_str(), 1);
  }
  std::string saved_path_;
};

// AN-01: announce with empty string is a no-op (no crash)
TEST_F(SilentAnnounceTest, EmptyString_NoOp) {
  EXPECT_NO_THROW(announce(""));
}

// AN-02: announce with blocking mode doesn't crash
TEST_F(SilentAnnounceTest, BlockingMode_NoCrash) {
  EXPECT_NO_THROW(announce("test", true));
}

// AN-03: announce with non-blocking mode doesn't crash
TEST_F(SilentAnnounceTest, NonBlockingMode_NoCrash) {
  EXPECT_NO_THROW(announce("test", false));
}
