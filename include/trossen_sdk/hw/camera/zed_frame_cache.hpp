/**
 * @file zed_frame_cache.hpp
 * @brief Frame cache for ZED SDK camera frames.
 */

#ifndef TROSSEN_SDK__HW__CAMERA__ZED_FRAME_CACHE_HPP
#define TROSSEN_SDK__HW__CAMERA__ZED_FRAME_CACHE_HPP

#include <memory>
#include <mutex>

#include <sl/Camera.hpp>

namespace trossen::hw::camera {

// ZED depth filtering confidence thresholds (0-100: lower=permissive, higher=strict)
constexpr int ZED_DEFAULT_CONFIDENCE_THRESHOLD = 50;          // General depth confidence
constexpr int ZED_DEFAULT_TEXTURE_CONFIDENCE_THRESHOLD = 100;  // Texture-based filtering

class ZedFrameCache {
public:
  /**
   * @brief Construct a ZED frame cache
   *
   * @param camera Shared pointer to ZED camera instance
   * @param num_consumers Expected number of consumers (e.g., 1 for color only, 2 for color+depth)
   */
  explicit ZedFrameCache(std::shared_ptr<sl::Camera> camera, size_t num_consumers)
    : camera_(std::move(camera)),
      expected_consumers_(num_consumers),
      consumed_(0),
      has_grabbed_(false) {}

  /**
   * @brief Get frames from the ZED camera with caching for multiple consumers
   *
   * The first consumer triggers camera->grab(), subsequent consumers reuse the result.
   * After all expected consumers have called this, the cache is cleared for the next frame.
   *
   * @return sl::ERROR_CODE from grab() operation
   */
  sl::ERROR_CODE grab() {
    std::lock_guard<std::mutex> lock(mutex_);

    // First consumer grabs new frame
    if (!has_grabbed_) {
      // Add runtime parameters for grab() - required for proper operation
      sl::RuntimeParameters runtime_params;
      runtime_params.confidence_threshold = ZED_DEFAULT_CONFIDENCE_THRESHOLD;
      runtime_params.texture_confidence_threshold = ZED_DEFAULT_TEXTURE_CONFIDENCE_THRESHOLD;

      last_error_ = camera_->grab(runtime_params);
      has_grabbed_ = true;
      consumed_ = 0;
    }

    ++consumed_;

    // Last consumer clears the cache
    if (consumed_ >= expected_consumers_) {
      has_grabbed_ = false;
      consumed_ = 0;
    }

    return last_error_;
  }

  /**
   * @brief Reset the frame cache state
   *
   * Call this when starting/stopping episodes to ensure clean state
   */
  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    has_grabbed_ = false;
    consumed_ = 0;
  }

  /**
   * @brief Get the underlying camera instance for retrieving images/depth
   *
   * @return Shared pointer to the sl::Camera
   */
  std::shared_ptr<sl::Camera> get_camera() const {
    return camera_;
  }

private:
  /// @brief ZED camera instance
  std::shared_ptr<sl::Camera> camera_;

  /// @brief Mutex to protect cache state
  std::mutex mutex_;

  /// @brief Number of expected consumers
  size_t expected_consumers_;

  /// @brief Number of consumers that have consumed the current frame
  size_t consumed_;

  /// @brief Whether grab() has been called for current frame
  bool has_grabbed_;

  /// @brief Last grab error code
  sl::ERROR_CODE last_error_;
};

}  // namespace trossen::hw::camera

#endif  // TROSSEN_SDK__HW__CAMERA__ZED_FRAME_CACHE_HPP
