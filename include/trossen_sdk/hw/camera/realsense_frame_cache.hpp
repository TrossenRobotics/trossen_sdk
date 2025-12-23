/**
 * @file realsense_frame_cache.hpp
 * @brief Frame cache for Realsense SDK for real hardware.
 */

#ifndef TROSSEN_SDK__HW__CAMERA__REALSENSE_FRAME_CACHE_HPP
#define TROSSEN_SDK__HW__CAMERA__REALSENSE_FRAME_CACHE_HPP

#include <memory>
#include <mutex>

#include "librealsense2/rs.hpp"

#include "trossen_sdk/hw/camera/realsense_frame_cache.hpp"


namespace trossen::hw::camera {

class RealsenseFrameCache {
public:
  explicit RealsenseFrameCache(std::shared_ptr<rs2::pipeline> pipeline,
                               size_t num_consumers)
    : pipeline_(std::move(pipeline)),
      expected_consumers_(num_consumers) {}

  /**
  * @brief Get frames from the Realsense pipeline with caching for multiple consumers.
  * @param timeout_ms Timeout in milliseconds to wait for frames.
  * @return Cached frameset shared among consumers.
  */
  rs2::frameset get_frames(int timeout_ms = 3000) {
    // TODO(shantanuparab-tr): Optimize locking strategy
    // remove mutex lock from hot path if possible
    std::lock_guard<std::mutex> lock(mutex_);

    if (!cached_) {
      cached_ = pipeline_->wait_for_frames(timeout_ms);
      consumed_ = 0;
    }

    ++consumed_;

    // Auto-clear when last consumer reads
    if (consumed_ >= expected_consumers_) {
      rs2::frameset out = cached_;
      cached_ = rs2::frameset();
      return out;
    }

    return cached_;
  }

private:
  /// @brief Realsense pipeline that provides frames
  std::shared_ptr<rs2::pipeline> pipeline_;

  /// @brief Mutex to protect cached frameset
  std::mutex mutex_;

  /// @brief Cached frameset to be shared among consumers
  rs2::frameset cached_;

  /// @brief Number of expected consumers
  size_t expected_consumers_;

  /// @brief Number of consumers that have consumed the current cached frameset
  size_t consumed_{0};
};


}  // namespace trossen::hw::camera

#endif  // TROSSEN_SDK__HW__CAMERA__REALSENSE_FRAME_CACHE_HPP
