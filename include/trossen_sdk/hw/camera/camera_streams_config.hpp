/**
 * @file camera_streams_config.hpp
 * @brief Unified configuration for (color [+ depth]) camera streams
 */

 #ifndef TROSSEN_SDK__HW__CAMERA__CAMERA_STREAMS_CONFIG_HPP
#define TROSSEN_SDK__HW__CAMERA__CAMERA_STREAMS_CONFIG_HPP

#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

#include "trossen_sdk/hw/camera/camera_types.hpp"

namespace trossen::hw::camera {

const int kMaxSupportedFrameRate = 120;
const int kMinSupportedFrameRate = 1;
const int kDefaultFrameRate = 30;
// Max 1080p resolution, default to 480p
const int kMaxSupportedHeight = 1080;
const int kMaxSupportedWidth = 1920;
const int kDefaultHeight = 480;
const int kDefaultWidth = 640;
const int kMinWarmupFrames = 0;
const int kDefaultWarmupFrames = 5;
const double kDefaultDepthScaleM = 1.0;  // 1m units for DEPTH16
const char kDefaultColorEncoding[] = "bgr8";
const DepthFormat kDefaultDepthFormat = DepthFormat::DEPTH16;
const CameraCapability kDefaultCameraCapability = CameraCapability::ColorOnly;
const bool kDefaultEnableDepth = false;

/**
 * @brief Configuration describing a logical camera that may emit only color or synchronized color+depth.
 *
 * Assumptions:
 *  - If depth enabled, color & depth share identical cadence (frame_rate) and timestamps.
 *  - A single sequence counter is applied to both frames in a pair.
 */
struct CameraStreamsConfig {
  /// @brief Camera capability profile
  CameraCapability capability{kDefaultCameraCapability};

  /// @brief Target frame rate (applies to both streams if depth enabled)
  int frame_rate{kDefaultFrameRate};

  /// @brief Color stream width in pixels
  int color_width{kDefaultWidth};

  /// @brief Color stream height in pixels
  int color_height{kDefaultHeight};

  /// @brief Depth stream width in pixels (if depth enabled)
  int depth_width{kDefaultWidth};

  /// @brief Depth stream height in pixels (if depth enabled)
  int depth_height{kDefaultHeight};

  /// @brief Color stream encoding (e.g. "bgr8", "rgb8", "mono8", etc)
  std::string color_encoding{kDefaultColorEncoding};

  /// @brief Depth stream pixel format (if depth enabled)
  DepthFormat depth_format{kDefaultDepthFormat};

  /// @brief Depth scale in meters per unit (if depth_format requires it, e.g. DEPTH16)
  double depth_scale_m{kDefaultDepthScaleM};

  /// @brief Number of initial frames to discard after starting the camera
  int warmup_frames{kDefaultWarmupFrames};

  /// @brief Convenience alias (if true -> capability ColorAndDepth)
  bool enable_depth{kDefaultEnableDepth};

  /**
   * @brief Apply normalization rules (idempotent).
   * - Reflect enable_depth into capability and vice-versa.
   * - Clamp 1 =< frame_rate =< 120.
   * - Ensure non-negative warmup.
   * - If DEPTH16 and depth_scale_m <= 0 reset to 1.0.
   */
  void normalize() {
    if (enable_depth) {
      capability = CameraCapability::ColorAndDepth;
    } else if (capability == CameraCapability::ColorOnly) {
      enable_depth = false;
    } else if (capability == CameraCapability::ColorAndDepth) {
      enable_depth = true;
    }

    if (frame_rate < kMinSupportedFrameRate) {
      frame_rate = kMinSupportedFrameRate;
    } else if (frame_rate > kMaxSupportedFrameRate) {
      frame_rate = kMaxSupportedFrameRate;
    }

    if (warmup_frames < kMinWarmupFrames) {
      warmup_frames = kMinWarmupFrames;
    }

    if (depth_format == DepthFormat::DEPTH16 && depth_scale_m <= 0.0) {
      depth_scale_m = kDefaultDepthScaleM;
    }

    // TODO(lukeschmitt-tr): validate resolutions
  }

  /**
   * @brief Validate invariants AFTER normalize().
   *
   * @param error optional string to receive human-readable error
   * @return true if valid
   */
  bool validate(std::string * error = nullptr) const {
    auto fail = [&](const std::string& msg) {
      if (error) {
        *error = msg;
      }
      return false;
    };

    if (color_width <= 0 || color_height <= 0) {
      return fail("color dimensions must be positive");
    }
    if (enable_depth) {
      if (depth_width <= 0 || depth_height <= 0) {
        return fail("depth dimensions must be positive when depth enabled");
      }
      if (depth_format == DepthFormat::DEPTH16 && depth_scale_m <= 0.0) {
        return fail("depth_scale_m must be > 0 for DEPTH16");
      }
    }
    if (frame_rate < 1 || frame_rate > 1000) {
      return fail("frame_rate out of supported range (1..1000)");
    }
    return true;
  }

  /**
   * @brief Estimated frame period in nanoseconds (ceil division to avoid drift accumulation)
   *
   * @return Frame period in nanoseconds
   */
  uint64_t frame_period_ns() const {
    // Use integer math: ceil(1e9 / fps) = (1e9 + fps - 1) / fps
    const uint64_t fps = static_cast<uint64_t>(frame_rate);
    return (1'000'000'000ULL + fps - 1ULL) / fps;
  }
};

}  // namespace trossen::hw::camera

#endif  // TROSSEN_SDK__HW__CAMERA__CAMERA_STREAMS_CONFIG_HPP
