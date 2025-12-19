#pragma once

#include <librealsense2/rs.hpp>
#include <memory>
#include <mutex>

namespace trossen::hw::camera {

class RealsenseFrameCache {
public:
  explicit RealsenseFrameCache(std::shared_ptr<rs2::pipeline> pipeline,
                               size_t num_consumers)
    : pipeline_(std::move(pipeline)),
      expected_consumers_(num_consumers) {}

  rs2::frameset get_frames(int timeout_ms = 3000) {
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
  std::shared_ptr<rs2::pipeline> pipeline_;
  std::mutex mutex_;

  rs2::frameset cached_;
  size_t expected_consumers_;
  size_t consumed_{0};
};


}  // namespace trossen::hw::camera
