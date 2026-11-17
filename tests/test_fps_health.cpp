/**
 * @file test_fps_health.cpp
 * @brief Unit tests for FpsHealth
 */

#include <cmath>
#include <cstdint>
#include <string>

#include "gtest/gtest.h"

#include "trossen_sdk/hw/camera/fps_health.hpp"

using trossen::hw::camera::FpsHealth;

namespace {

/// @brief Feed frames at `fps` for `seconds` starting at `start_ns`; returns the warnings.
int feed(FpsHealth& health, uint64_t start_ns, double fps, double seconds) {
  int warnings = 0;
  const auto frames = static_cast<int>(std::lround(fps * seconds));
  for (int i = 0; i <= frames; ++i) {
    const auto t = start_ns + static_cast<uint64_t>(std::llround(i * 1e9 / fps));
    if (health.on_frame(t)) ++warnings;
  }
  return warnings;
}

constexpr uint64_t kSecondNs{1'000'000'000};

}  // namespace

// The time between two episodes is not counted once restart() is called.
TEST(FpsHealth, RestartIgnoresTheGapBetweenEpisodes) {
  FpsHealth health{"camera_top", 30};
  EXPECT_EQ(feed(health, kSecondNs, 30.0, 10.0), 0);

  health.restart();
  EXPECT_EQ(feed(health, 16 * kSecondNs, 30.0, 10.0), 0);
}

// A frame or two lost in 10 s is normal jitter and prints nothing.
TEST(FpsHealth, SlightlyLowRateLogsNothing) {
  FpsHealth health{"camera_top", 30};
  testing::internal::CaptureStderr();
  const int warnings = feed(health, kSecondNs, 29.8, 10.0);
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
  EXPECT_EQ(warnings, 0);
}

// A rate below 93% of the requested fps warns once per window, naming the camera.
TEST(FpsHealth, LowRateWarnsOnceNamingTheCamera) {
  FpsHealth health{"camera_top", 30};
  testing::internal::CaptureStderr();
  const int warnings = feed(health, kSecondNs, 27.5, 10.0);
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_EQ(warnings, 1);
  EXPECT_NE(err.find("camera 'camera_top'"), std::string::npos) << err;
}
