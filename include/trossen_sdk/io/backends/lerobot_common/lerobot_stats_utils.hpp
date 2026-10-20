/**
 * @file lerobot_stats_utils.hpp
 * @brief Statistics and image-sampling utilities shared by every LeRobot output version.
 *
 * These operate on Arrow arrays and OpenCV images and are independent of the
 * dataset layout (v2.1 vs v3.0).
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_STATS_UTILS_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_STATS_UTILS_HPP_

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

#include <arrow/api.h>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

namespace trossen::io::backends {

/**
 * @brief Running min, max, mean and population standard deviation of a stream of values.
 *
 * A constant stream gets a std of exactly 0.
 */
struct RunningStats {
  /// @brief Number of values added.
  int64_t count{0};
  /// @brief Mean of the values added so far.
  double running_mean{0.0};
  /// @brief Sum of squared deviations from the running mean.
  double m2{0.0};
  /// @brief Smallest value added.
  double lo{std::numeric_limits<double>::max()};
  /// @brief Largest value added.
  double hi{std::numeric_limits<double>::lowest()};

  /**
   * @brief Add one value.
   * @param v Value to add.
   */
  void add(double v);

  /// @brief Mean of the values. @return The mean, or 0 when no value was added.
  double mean() const;
  /// @brief Smallest value. @return The minimum, or 0 when no value was added.
  double min() const;
  /// @brief Largest value. @return The maximum, or 0 when no value was added.
  double max() const;
  /// @brief Population standard deviation. @return The std, or 0 when no value was added.
  double stddev() const;
};

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
 * @brief Compute statistics for a set of images
 *
 * @param images Vector of OpenCV Mat objects representing the images
 * @return JSON object containing the computed statistics
 */
nlohmann::ordered_json compute_image_stats(const std::vector<cv::Mat> &images);

/**
 * @brief Sample a set of images from a list of image paths
 *
 * @param image_paths Vector of filesystem paths to the images
 * @return Vector of OpenCV Mat objects representing the sampled images
 */
std::vector<cv::Mat> sample_images(
  const std::vector<std::filesystem::path> &image_paths);

/**
 * @brief Automatically downsample an image to a target size
 *
 * @param img OpenCV Mat object representing the image
 * @param target_size Target size for the downsampled image (default is 150)
 * @param max_threshold Maximum threshold for downsampling (default is 300)
 * @return Downsampled OpenCV Mat object
 */
cv::Mat auto_downsample(
  const cv::Mat &img,
  int target_size = 150,
  int max_threshold = 300);

/**
 * @brief Sample indices for selecting images from a dataset
 *
 * @param dataset_len Length of the dataset
 * @param min_samples Minimum number of samples to select (default is 100)
 * @param max_samples Maximum number of samples to select (default is 10000)
 * @param power Power factor for sampling distribution (default is 0.75)
 * @return Vector of sampled indices
 */
std::vector<int> sample_indices(
  int dataset_len,
  int min_samples = 100,
  int max_samples = 10000,
  float power = 0.75f);

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_STATS_UTILS_HPP_
