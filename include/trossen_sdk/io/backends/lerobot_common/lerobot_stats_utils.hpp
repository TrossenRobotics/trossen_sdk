/**
 * @file lerobot_stats_utils.hpp
 * @brief Statistics and image-sampling utilities shared by every LeRobot output version.
 *
 * These operate on Arrow arrays and OpenCV images and are independent of the
 * dataset layout (v2.1 vs v3.0).
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
 * @brief Compute statistics for a ListArray
 *
 * @param list_array Shared pointer to the ListArray
 * @return JSON object containing the computed statistics
 */
nlohmann::ordered_json compute_list_stats(
  const std::shared_ptr<arrow::ListArray> &list_array);

/**
 * @brief Compute statistics for a flat array
 *
 * @param array Shared pointer to the flat array
 * @return JSON object containing the computed statistics
 */
nlohmann::ordered_json compute_flat_stats(const std::shared_ptr<arrow::Array> &array);

/**
 * @brief Compute statistics for a FixedSizeListArray
 *
 * @param fixed_list_array Shared pointer to the FixedSizeListArray
 * @return JSON object containing the computed statistics
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
