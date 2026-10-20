/**
 * @file lerobot_stats_utils.hpp
 * @brief Statistics and image-sampling utilities shared by every LeRobot output version.
 *
 * These operate on Arrow arrays and OpenCV images and are independent of the
 * dataset layout (v2.1 vs v3.0). They follow LeRobot 0.6.0's
 * `lerobot/datasets/compute_stats.py`: the same sample count, the same strided
 * downsampling, population standard deviation, and image statistics per RGB channel.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_STATS_UTILS_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_STATS_UTILS_HPP_

#include <filesystem>
#include <memory>
#include <vector>

#include <arrow/api.h>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

namespace trossen::io::backends {

/**
 * @brief Compute per-dimension statistics for a ListArray column
 *
 * Every non-null row must hold the same number of values, as LeRobot requires of a
 * vector feature, and no null values. Null rows are left out.
 *
 * @param list_array Shared pointer to the ListArray; its values must be numeric
 * @return JSON object with min, max, mean, std (one entry per dimension) and count (rows)
 * @throws std::invalid_argument If the rows differ in length, a row holds a null value, or
 *   the values are not numeric
 */
nlohmann::ordered_json compute_list_stats(
  const std::shared_ptr<arrow::ListArray> &list_array);

/**
 * @brief Compute statistics for a flat numeric array
 *
 * Null values are left out. An array with no values gives zeros and a count of 0.
 *
 * @param array Shared pointer to the flat array; float, double or int64
 * @return JSON object with min, max, mean, std and count, each a one-element list
 * @throws std::invalid_argument For any other value type
 */
nlohmann::ordered_json compute_flat_stats(const std::shared_ptr<arrow::Array> &array);

/**
 * @brief Compute per-dimension statistics for a FixedSizeListArray column
 *
 * Null rows are left out. The result has one entry per dimension of the list type, even
 * when no row is left.
 *
 * @param fixed_list_array Shared pointer to the FixedSizeListArray; its values must be numeric
 * @return JSON object with min, max, mean, std (one entry per dimension) and count (rows)
 * @throws std::invalid_argument If a row holds a null value or the values are not numeric
 */
nlohmann::ordered_json compute_fixed_size_list_stats(
  const std::shared_ptr<arrow::FixedSizeListArray> &fixed_list_array);

/**
 * @brief Compute per-channel statistics for a set of images
 *
 * Statistics are reported in RGB order with shape [3, 1, 1], matching LeRobot. Images
 * that are not CV_32FC3 are skipped with a warning.
 *
 * @param images Samples from sample_images(): CV_32FC3, BGR, scaled to [0, 1]
 * @return JSON object with min, max, mean, std and count (images used)
 */
nlohmann::ordered_json compute_image_stats(const std::vector<cv::Mat> &images);

/**
 * @brief Load the images at sample_indices() and downsample them
 *
 * @param image_paths Paths to every frame of one camera, in frame order
 * @return The sampled images as CV_32FC3 in OpenCV's BGR order, scaled to [0, 1];
 *   unreadable files are skipped
 */
std::vector<cv::Mat> sample_images(
  const std::vector<std::filesystem::path> &image_paths);

/**
 * @brief Downsample an image by keeping every n-th pixel
 *
 * Matches LeRobot's `auto_downsample_height_width()`: an image whose larger side is
 * below max_threshold is returned as is; otherwise n is the larger side divided by
 * target_size, rounded down, and every n-th row and column is kept.
 *
 * @param img Image to downsample
 * @param target_size Larger side to aim for (default 150)
 * @param max_threshold Larger side from which downsampling starts (default 300)
 * @return The downsampled image, or img itself when no downsampling is needed
 */
cv::Mat auto_downsample(
  const cv::Mat &img,
  int target_size = 150,
  int max_threshold = 300);

/**
 * @brief Pick evenly spaced frame indices to sample
 *
 * Matches LeRobot's `sample_indices()`: the count is dataset_len^power clamped to
 * [min(min_samples, dataset_len), max_samples], and the indices are spread evenly from 0
 * to dataset_len - 1. With a power below 1, as by default, no frame is sampled twice.
 *
 * @param dataset_len Number of frames
 * @param min_samples Minimum number of samples (default 100)
 * @param max_samples Maximum number of samples (default 10000)
 * @param power Growth of the sample count with dataset size (default 0.75)
 * @return The sampled indices, in increasing order; empty when dataset_len is 0
 */
std::vector<int> sample_indices(
  int dataset_len,
  int min_samples = 100,
  int max_samples = 10000,
  float power = 0.75f);

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_STATS_UTILS_HPP_
