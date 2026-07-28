/**
 * @file camera_utils.hpp
 * @brief Shared helpers for camera frame handling (validation, preview capture, ...).
 */

#ifndef TROSSEN_SDK__UTILS__CAMERA_UTILS_HPP_
#define TROSSEN_SDK__UTILS__CAMERA_UTILS_HPP_

#include "opencv2/core.hpp"

namespace trossen::utils {

/// @brief Overall mean channel value below which a frame is treated as "black."
constexpr double kPreviewMinMean = 10.0;

/// @brief Overall mean channel value above which a frame is treated as "saturated."
constexpr double kPreviewMaxMean = 245.0;

/// @brief Per-channel std-dev below which a frame is treated as "flat fill."
///
/// Catches the "all green" / "all magenta" buffers some drivers return during
/// pipeline startup before the sensor has actually produced a frame.
constexpr double kPreviewMinStdDev = 3.0;

/// @brief Default cap on the number of frames a discovery warmup loop will
///        read while waiting for a valid, stable frame.
///
/// At 30 FPS this is ~3 s, which covers cold-boot auto-exposure on the
/// longest-settling sensors we ship with.
constexpr int kPreviewMaxWarmupFrames = 90;

/**
 * @brief Decide whether @p frame is a trustworthy preview / discovery frame.
 *
 * Rejects empty frames, frames that are near-uniformly dark or bright, and
 * frames with negligible per-channel variation. Use this to gate exit from a
 * warmup loop so a degenerate startup frame is not committed as the camera's
 * preview.
 */
bool is_valid_preview_frame(const cv::Mat& frame);

}  // namespace trossen::utils

#endif  // TROSSEN_SDK__UTILS__CAMERA_UTILS_HPP_
