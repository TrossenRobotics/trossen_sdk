/**
 * @file fps_health.hpp
 * @brief Warns when a camera delivers frames below its requested rate.
 */

#ifndef TROSSEN_SDK__HW__CAMERA__FPS_HEALTH_HPP_
#define TROSSEN_SDK__HW__CAMERA__FPS_HEALTH_HPP_

#include <cstdint>
#include <string>

namespace trossen::hw::camera {

/**
 * @brief Measures a camera's frame rate over fixed windows and warns when it is low
 *
 * Not thread-safe: on_frame() and restart() are called from the thread that captures frames.
 */
class FpsHealth {
public:
  /// @brief Length of each measurement window, in nanoseconds (10 s)
  static constexpr uint64_t kWindowNs{10'000'000'000};

  /// @brief Fraction of the requested fps below which a window warns
  static constexpr double kMinFraction{0.93};

  /**
   * @brief Construct a monitor for one camera
   *
   * @param camera_id Stream id named in the warning
   * @param requested_fps Frame rate the camera was configured for; 0 disables the check
   */
  FpsHealth(std::string camera_id, int requested_fps);

  /**
   * @brief Drop the current window; the next frame starts a new one
   *
   * Call at each episode start, so the time between episodes is not measured.
   */
  void restart();

  /**
   * @brief Count one frame, and warn on stderr if it closes a window whose rate is low
   *
   * @param mono_ns Capture time of the frame, from a monotonic clock, in nanoseconds
   * @return True if this frame closed a window and a warning was printed
   */
  bool on_frame(uint64_t mono_ns);

private:
  /// @brief Stream id named in the warning
  std::string camera_id_;

  /// @brief Requested frame rate, in frames per second
  int requested_fps_;

  /// @brief Capture time of the frame that opened the window, in ns; 0 when no window is open
  uint64_t window_start_ns_{0};

  /// @brief Capture time of the previous frame, in ns
  uint64_t last_frame_ns_{0};

  /// @brief Frames counted since the window opened, not counting the first
  uint64_t frames_{0};

  /// @brief Longest interval between two frames in the window, in ns
  uint64_t max_interval_ns_{0};
};

}  // namespace trossen::hw::camera

#endif  // TROSSEN_SDK__HW__CAMERA__FPS_HEALTH_HPP_
