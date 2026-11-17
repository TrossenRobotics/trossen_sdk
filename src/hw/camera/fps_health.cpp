/**
 * @file fps_health.cpp
 * @brief Implementation of FpsHealth
 */

#include <iostream>
#include <string>
#include <utility>

#include "trossen_sdk/hw/camera/fps_health.hpp"

namespace trossen::hw::camera {

FpsHealth::FpsHealth(std::string camera_id, int requested_fps)
  : camera_id_{std::move(camera_id)}, requested_fps_{requested_fps} {}

void FpsHealth::restart() {
  window_start_ns_ = 0;
}

bool FpsHealth::on_frame(uint64_t mono_ns) {
  if (requested_fps_ <= 0) {
    return false;
  }
  if (window_start_ns_ == 0) {
    window_start_ns_ = mono_ns;
    last_frame_ns_ = mono_ns;
    frames_ = 0;
    max_interval_ns_ = 0;
    return false;
  }

  ++frames_;
  if (mono_ns - last_frame_ns_ > max_interval_ns_) {
    max_interval_ns_ = mono_ns - last_frame_ns_;
  }
  last_frame_ns_ = mono_ns;

  const uint64_t elapsed_ns = mono_ns - window_start_ns_;
  if (elapsed_ns < kWindowNs) {
    return false;
  }

  // Intervals per second over the window: N frames after the opening one span N intervals.
  const double fps = static_cast<double>(frames_) * 1e9 / static_cast<double>(elapsed_ns);
  const bool low = fps < kMinFraction * requested_fps_;
  if (low) {
    std::cerr << "camera '" << camera_id_ << "': " << fps << " fps over the last "
              << elapsed_ns / 1e9 << " s, below the requested " << requested_fps_
              << " fps; longest gap " << max_interval_ns_ / 1e6
              << " ms. Check the USB connection, bandwidth and CPU load" << std::endl;
  }
  window_start_ns_ = mono_ns;
  frames_ = 0;
  max_interval_ns_ = 0;
  return low;
}

}  // namespace trossen::hw::camera
