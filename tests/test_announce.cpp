/**
 * @file test_announce.cpp
 * @brief Unit tests for the announce() text-to-speech utility
 *
 * Tests verify graceful behavior (no crash) without producing audio: PATH is either
 * cleared, so posix_spawnp cannot find spd-say, or points at a fake spd-say.
 */

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// AN-02: announce with a message doesn't crash when spd-say cannot be found
TEST_F(SilentAnnounceTest, MissingBinary_NoCrash) {
  EXPECT_NO_THROW(announce("test"));
}

// Puts a fake spd-say first on PATH. It records its arguments and, like the real one, keeps
// running while speech plays when given -w (here 5 s), so a call that waits on speech is caught.
class FakeSpdSayTest : public ::testing::Test {
protected:
  void SetUp() override {
    const char* p = std::getenv("PATH");
    saved_path_ = p ? p : "";
    dir_ = std::filesystem::temp_directory_path() /
           ("fake_spd_say_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir_);
    args_file_ = dir_ / "args";
    const auto script = dir_ / "spd-say";
    std::ofstream(script) << "#!/bin/sh\n"
                          << "echo \"$@\" > '" << args_file_.string() << "'\n"
                          << "for a in \"$@\"; do [ \"$a\" = -w ] && sleep 5; done\n"
                          << "exit 0\n";
    std::filesystem::permissions(script, std::filesystem::perms::owner_all);
    setenv("PATH", (dir_.string() + ":/usr/bin:/bin").c_str(), 1);
  }
  void TearDown() override {
    setenv("PATH", saved_path_.c_str(), 1);
    std::filesystem::remove_all(dir_);
  }
  std::string recorded_args() const {
    std::ifstream in(args_file_);
    std::string line;
    std::getline(in, line);
    return line;
  }
  std::string saved_path_;
  std::filesystem::path dir_;
  std::filesystem::path args_file_;
};

// AN-03: announce submits the message without -w and returns once spd-say exits
TEST_F(FakeSpdSayTest, SubmitsWithoutWaitingOnSpeech) {
  const auto start = std::chrono::steady_clock::now();
  announce("test");
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 2);
  EXPECT_EQ(recorded_args(), "test");
}
