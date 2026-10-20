/**
 * @file lerobot_stats_utils.cpp
 * @brief Implementation of the shared LeRobot stats/image-sampling utilities.
 */

#include "trossen_sdk/io/backends/lerobot_common/lerobot_stats_utils.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace trossen::io::backends {

namespace {

/**
 * @brief Running min, max, mean and population standard deviation of a stream of values.
 *
 * Uses Welford's update in double. Its squared-deviation sum cannot go negative, so a
 * constant column (such as episode_index within one episode) gets a std of exactly 0,
 * where the textbook `sum_sq / n - mean^2` form can come out slightly negative and make
 * sqrt() NaN. LeRobot 0.6.0 clamps that form at 0 instead.
 */
struct RunningStats {
  int64_t count{0};
  double running_mean{0.0};
  double m2{0.0};
  double lo{std::numeric_limits<double>::max()};
  double hi{std::numeric_limits<double>::lowest()};

  void add(double v) {
    ++count;
    const double delta = v - running_mean;
    running_mean += delta / static_cast<double>(count);
    m2 += delta * (v - running_mean);
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }

  double mean() const { return count > 0 ? running_mean : 0.0; }
  double min() const { return count > 0 ? lo : 0.0; }
  double max() const { return count > 0 ? hi : 0.0; }
  double stddev() const {
    return count > 0 ? std::sqrt(m2 / static_cast<double>(count)) : 0.0;
  }
};

/**
 * @brief Element i of a float, double or int64 array as a double.
 * @throws std::invalid_argument For any other value type.
 */
double numeric_at(const arrow::Array &array, int64_t i) {
  switch (array.type_id()) {
    case arrow::Type::FLOAT:
      return static_cast<const arrow::FloatArray &>(array).Value(i);
    case arrow::Type::DOUBLE:
      return static_cast<const arrow::DoubleArray &>(array).Value(i);
    case arrow::Type::INT64:
      return static_cast<double>(static_cast<const arrow::Int64Array &>(array).Value(i));
    default:
      throw std::invalid_argument("stats: values must be float, double or int64, got " +
                                  array.type()->ToString());
  }
}

}  // namespace

std::vector<int> sample_indices(
  int dataset_len,
  int min_samples,
  int max_samples,
  float power)
{
  // Calculate the number of samples based on the power law
  // Clamp the number of samples between min_samples and max_samples
  int num_samples = std::clamp(
    static_cast<int>(std::pow(dataset_len, power)),
    min_samples,
    max_samples);

  std::vector<int> indices(num_samples);
  float step = static_cast<float>(dataset_len - 1) / (num_samples - 1);
  for (int i = 0; i < num_samples; ++i) indices[i] = std::round(i * step);
  return indices;
}

cv::Mat auto_downsample(
  const cv::Mat &img,
  int target_size,
  int max_threshold)
{
  int h = img.rows;
  int w = img.cols;
  // If the larger dimension is already below the max threshold, return the original image
  if (std::max(w, h) < max_threshold) return img;
  // Calculate the downsampling factor to make the larger dimension equal to target_size
  float factor = (w > h) ? (w / static_cast<float>(target_size))
                        : (h / static_cast<float>(target_size));
  // Downsample the image using area interpolation for better quality
  cv::Mat downsampled;
  cv::resize(img, downsampled, {}, 1.0 / factor, 1.0 / factor, cv::INTER_AREA);
  return downsampled;
}

std::vector<cv::Mat> sample_images(
  const std::vector<std::filesystem::path> &image_paths)
{
  if (image_paths.empty()) {
    return {};
  }

  // Sample indices using power law distribution
  auto indices = sample_indices(image_paths.size());

  std::vector<cv::Mat> images;
  // Load and process images at the sampled indices
  for (int idx : indices) {
    cv::Mat img = cv::imread(image_paths[idx].string(), cv::IMREAD_COLOR);
    if (img.empty()) continue;
    // Downsample the image if necessary and convert to float32
    cv::Mat img_downsampled = auto_downsample(img);
    cv::Mat img_float;
    // Normalize pixel values to [0, 1]
    img_downsampled.convertTo(img_float, CV_32F, 1.0 / 255.0);
    // Ensure the image has 3 channels (BGR)
    if (img_float.channels() == 3) {
      images.push_back(img_float);
    } else {
      std::cerr << "Unexpected channel count: " << img_float.channels() << std::endl;
    }
  }

  return images;
}

nlohmann::ordered_json compute_image_stats(
  const std::vector<cv::Mat> &images)
{
  nlohmann::ordered_json stats_json;
  if (images.empty()) {
    std::cerr << "No images provided." << std::endl;
    stats_json["min"] = {};
    stats_json["max"] = {};
    stats_json["mean"] = {};
    stats_json["std"] = {};
    stats_json["count"] = {0};
    return stats_json;
  }

  int num_channels = images[0].channels();
  int count = static_cast<int>(images.size());

  // Create a vector for each channel
  std::vector<std::vector<float>> channel_values(num_channels);

  for (const auto &img : images) {
    std::vector<cv::Mat> channels;
    cv::split(img, channels);

    for (int c = 0; c < num_channels; ++c) {
      // Flatten and push pixels into channel_values[c]
      channel_values[c].insert(
        channel_values[c].end(),
        reinterpret_cast<float *>(const_cast<uchar *>(channels[c].datastart)),
        reinterpret_cast<float *>(const_cast<uchar *>(channels[c].dataend)));
    }
  }

  // Helper lambda to convert a vector to a nested JSON array
  auto to_nested = [](const std::vector<float> &vec) {
    nlohmann::ordered_json result = nlohmann::ordered_json::array();
    for (float v : vec) {
      result.push_back({{v}});
    }
    return result;
  };

  std::vector<float> min_vals, max_vals, mean_vals, std_vals;
  for (int c = 0; c < num_channels; ++c) {
    cv::Mat channel_mat(channel_values[c]);
    cv::Scalar mean, stddev;
    cv::meanStdDev(channel_mat, mean, stddev);

    double min_val, max_val;
    cv::minMaxLoc(channel_mat, &min_val, &max_val);

    min_vals.push_back(static_cast<float>(min_val));
    max_vals.push_back(static_cast<float>(max_val));
    mean_vals.push_back(static_cast<float>(mean[0]));
    std_vals.push_back(static_cast<float>(stddev[0]));
  }

  stats_json["min"] = to_nested(min_vals);
  stats_json["max"] = to_nested(max_vals);
  stats_json["mean"] = to_nested(mean_vals);
  stats_json["std"] = to_nested(std_vals);
  stats_json["count"] = {count};

  return stats_json;
}

nlohmann::ordered_json compute_flat_stats(
  const std::shared_ptr<arrow::Array> &array)
{
  RunningStats stats;
  for (int64_t i = 0; i < array->length(); ++i) {
    if (array->IsNull(i)) continue;
    stats.add(numeric_at(*array, i));
  }
  return {{"min", {stats.min()}},
          {"max", {stats.max()}},
          {"mean", {stats.mean()}},
          {"std", {stats.stddev()}},
          {"count", {stats.count}}};
}

nlohmann::ordered_json compute_list_stats(
  const std::shared_ptr<arrow::ListArray> &list_array)
{
  // Get the values array from the ListArray
  // Handle both Float and Double arrays
  auto values_array = list_array->values();

  // Calculate the number of lists and the dimension of each list
  int64_t list_count = list_array->length();
  int64_t value_count = values_array->length();
  int64_t dim = list_count > 0 ? value_count / list_count : 0;

  // Initialize statistics vectors
  std::vector<double> sum(dim, 0.0), sum_sq(dim, 0.0),
      min_val(dim, std::numeric_limits<double>::max()),
      max_val(dim, std::numeric_limits<double>::lowest());

  // Iterate over the values and compute statistics for each dimension
  for (int64_t i = 0; i < value_count; ++i) {
    double val = 0.0;

    // Handle both Float32 and Float64 types
    if (values_array->type_id() == arrow::Type::FLOAT) {
      val = static_cast<double>(
          std::static_pointer_cast<arrow::FloatArray>(values_array)->Value(i));
    } else if (values_array->type_id() == arrow::Type::DOUBLE) {
      val = std::static_pointer_cast<arrow::DoubleArray>(values_array)->Value(i);
    } else {
      // Unsupported type, skip
      continue;
    }

    int d = i % dim;
    min_val[d] = std::min(min_val[d], val);
    max_val[d] = std::max(max_val[d], val);
    sum[d] += val;
    sum_sq[d] += val * val;
  }

  std::vector<double> mean(dim), stddev(dim);
  // Compute mean and standard deviation for each dimension
  for (int d = 0; d < dim; ++d) {
    mean[d] = sum[d] / list_count;
    // Handle edge case where all values are zero and standard deviation can be NaN
    double variance = (sum_sq[d] / list_count) - (mean[d] * mean[d]);
    stddev[d] = (list_count > 0 && variance >= 0.0) ? std::sqrt(variance) : 0.0;
  }

  return {{"min", min_val},
          {"max", max_val},
          {"mean", mean},
          {"std", stddev},
          {"count", {list_count}}};
}

nlohmann::ordered_json compute_fixed_size_list_stats(
  const std::shared_ptr<arrow::FixedSizeListArray> &fixed_list_array)
{
  auto values_array = fixed_list_array->values();

  int64_t list_count = fixed_list_array->length();
  int64_t value_count = values_array->length();
  int64_t dim = list_count > 0 ? value_count / list_count : 0;

  std::vector<double> sum(dim, 0.0), sum_sq(dim, 0.0),
      min_val(dim, std::numeric_limits<double>::max()),
      max_val(dim, std::numeric_limits<double>::lowest());

  // Iterate and compute stats
  for (int64_t i = 0; i < value_count; ++i) {
    double val = 0.0;
    // Handle different numeric types (float and double)
    if (values_array->type_id() == arrow::Type::FLOAT) {
      val = static_cast<double>(
          std::static_pointer_cast<arrow::FloatArray>(values_array)->Value(i));
    } else if (values_array->type_id() == arrow::Type::DOUBLE) {
      val = std::static_pointer_cast<arrow::DoubleArray>(values_array)->Value(i);
    }

    int d = i % dim;
    min_val[d] = std::min(min_val[d], val);
    max_val[d] = std::max(max_val[d], val);
    sum[d] += val;
    sum_sq[d] += val * val;
  }
  // Compute mean and standard deviation for each dimension
  std::vector<double> mean(dim), stddev(dim);
  for (int d = 0; d < dim; ++d) {
    mean[d] = sum[d] / list_count;
    double variance = (sum_sq[d] / list_count) - (mean[d] * mean[d]);
    stddev[d] = (list_count > 0 && variance >= 0.0) ? std::sqrt(variance) : 0.0;
  }
  // Store stats in JSON
  return {{"min", min_val},
          {"max", max_val},
          {"mean", mean},
          {"std", stddev},
          {"count", {list_count}}};
}

}  // namespace trossen::io::backends
