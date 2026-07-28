/**
 * @file camera_utils.cpp
 * @brief Implementation of shared camera utilities.
 */

#include "trossen_sdk/utils/camera_utils.hpp"

#include <limits>

namespace trossen::utils {

bool is_valid_preview_frame(const cv::Mat& frame) {
  if (frame.empty()) return false;

  cv::Scalar mean;
  cv::Scalar stddev;
  cv::meanStdDev(frame, mean, stddev);

  const int channels = frame.channels();

  // Overall mean across channels gauges "is the frame dark or saturated?"
  double total_mean = 0.0;
  for (int c = 0; c < channels; ++c) total_mean += mean[c];
  total_mean /= channels;
  if (total_mean < kPreviewMinMean) return false;
  if (total_mean > kPreviewMaxMean) return false;

  // Worst-case per-channel std-dev catches flat-fill buffers: a uniform green
  // frame has mean=~85 (passes the range above) but std-dev=0 in every channel.
  double min_stddev = std::numeric_limits<double>::max();
  for (int c = 0; c < channels; ++c) {
    if (stddev[c] < min_stddev) min_stddev = stddev[c];
  }
  if (min_stddev < kPreviewMinStdDev) return false;

  return true;
}

}  // namespace trossen::utils
