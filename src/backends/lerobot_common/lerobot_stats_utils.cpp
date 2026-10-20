/**
 * @file lerobot_stats_utils.cpp
 * @brief Implementation of the shared LeRobot stats/image-sampling utilities.
 */

#include "trossen_sdk/io/backends/lerobot_common/lerobot_stats_utils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <type_traits>

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

/**
 * @brief Per-dimension statistics over the rows of a list column.
 *
 * Rows are read through their own offsets, so a sliced array is handled. Every non-null
 * row must hold the same number of values, none of them null.
 *
 * @tparam ListArrayT arrow::ListArray or arrow::FixedSizeListArray.
 */
template <typename ListArrayT>
nlohmann::ordered_json list_stats(const ListArrayT &lists, const char *caller) {
  const std::shared_ptr<arrow::Array> values = lists.values();

  int64_t dim = -1;
  int64_t rows = 0;
  std::vector<RunningStats> per_dim;
  // A fixed-size list declares its width, so the stats keep that shape with no rows.
  if constexpr (std::is_same_v<ListArrayT, arrow::FixedSizeListArray>) {
    dim = lists.list_type()->list_size();
    per_dim.resize(static_cast<size_t>(dim));
  }
  for (int64_t r = 0; r < lists.length(); ++r) {
    if (lists.IsNull(r)) continue;
    const int64_t len = lists.value_length(r);
    if (dim < 0) {
      dim = len;
      per_dim.resize(static_cast<size_t>(dim));
    } else if (len != dim) {
      throw std::invalid_argument(std::string(caller) + ": row " + std::to_string(r) + " holds " +
                                  std::to_string(len) + " values but earlier rows hold " +
                                  std::to_string(dim));
    }
    const int64_t start = lists.value_offset(r);
    for (int64_t k = 0; k < len; ++k) {
      // Every dimension shares the row count, so a row missing a value cannot be counted.
      if (values->IsNull(start + k)) {
        throw std::invalid_argument(std::string(caller) + ": row " + std::to_string(r) +
                                    " holds a null value");
      }
      per_dim[static_cast<size_t>(k)].add(numeric_at(*values, start + k));
    }
    ++rows;
  }

  std::vector<double> min_val, max_val, mean, stddev;
  for (const auto &s : per_dim) {
    min_val.push_back(s.min());
    max_val.push_back(s.max());
    mean.push_back(s.mean());
    stddev.push_back(s.stddev());
  }
  return {{"min", min_val},
          {"max", max_val},
          {"mean", mean},
          {"std", stddev},
          {"count", {rows}}};
}

}  // namespace

std::vector<int> sample_indices(
  int dataset_len,
  int min_samples,
  int max_samples,
  float power)
{
  if (dataset_len <= 0) return {};

  // LeRobot's estimate_num_samples(): dataset_len^power clamped to
  // [min_samples, max_samples], where min_samples drops to dataset_len for a short
  // dataset so no frame is sampled twice.
  const int lower = std::min(min_samples, dataset_len);
  const int by_power = static_cast<int>(
    std::pow(static_cast<double>(dataset_len), static_cast<double>(power)));
  const int num_samples = std::max(lower, std::min(by_power, max_samples));

  std::vector<int> indices(num_samples);
  if (num_samples == 1) {
    indices[0] = 0;
    return indices;
  }
  // np.round(np.linspace(0, dataset_len - 1, num_samples)). np.round rounds halves to
  // even, as std::nearbyint does under the default rounding mode.
  const double step = static_cast<double>(dataset_len - 1) / (num_samples - 1);
  for (int i = 0; i < num_samples; ++i) {
    indices[i] = static_cast<int>(std::nearbyint(i * step));
  }
  return indices;
}

cv::Mat auto_downsample(
  const cv::Mat &img,
  int target_size,
  int max_threshold)
{
  const int h = img.rows;
  const int w = img.cols;
  // If the larger dimension is already below the max threshold, return the original image
  if (std::max(w, h) < max_threshold || target_size <= 0) return img;

  // LeRobot keeps every factor-th row and column (img[:, ::factor, ::factor]) rather than
  // interpolating, with factor = larger side // target_size.
  const int factor = std::max(w, h) / target_size;
  if (factor <= 1) return img;
  cv::Mat downsampled((h + factor - 1) / factor, (w + factor - 1) / factor, img.type());
  const size_t pixel_bytes = img.elemSize();
  for (int y = 0; y < downsampled.rows; ++y) {
    const uchar *src = img.ptr(y * factor);
    uchar *dst = downsampled.ptr(y);
    for (int x = 0; x < downsampled.cols; ++x) {
      std::memcpy(dst + x * pixel_bytes, src + static_cast<size_t>(x) * factor * pixel_bytes,
                  pixel_bytes);
    }
  }
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
  // LeRobot reports image statistics per channel in RGB order; OpenCV images are BGR.
  std::array<RunningStats, 3> rgb;
  int64_t count = 0;
  for (const auto &img : images) {
    // Reading anything but float32 pixels as floats would turn the bytes into noise.
    if (img.type() != CV_32FC3) {
      std::cerr << "Warning: skipping image sample with unexpected type " << img.type()
                << " (expected CV_32FC3)\n";
      continue;
    }
    for (int y = 0; y < img.rows; ++y) {
      const cv::Vec3f *row = img.ptr<cv::Vec3f>(y);
      for (int x = 0; x < img.cols; ++x) {
        rgb[0].add(row[x][2]);
        rgb[1].add(row[x][1]);
        rgb[2].add(row[x][0]);
      }
    }
    ++count;
  }

  nlohmann::ordered_json stats_json;
  if (count == 0) {
    std::cerr << "Warning: no CV_32FC3 image among the " << images.size()
              << " provided; image statistics are empty\n";
    stats_json["min"] = {};
    stats_json["max"] = {};
    stats_json["mean"] = {};
    stats_json["std"] = {};
    stats_json["count"] = {0};
    return stats_json;
  }

  // Shape [3, 1, 1]: one [[value]] per channel.
  auto per_channel = [&rgb](auto value_of) {
    nlohmann::ordered_json result = nlohmann::ordered_json::array();
    for (const auto &channel : rgb) result.push_back({{value_of(channel)}});
    return result;
  };
  stats_json["min"] = per_channel([](const RunningStats &s) { return s.min(); });
  stats_json["max"] = per_channel([](const RunningStats &s) { return s.max(); });
  stats_json["mean"] = per_channel([](const RunningStats &s) { return s.mean(); });
  stats_json["std"] = per_channel([](const RunningStats &s) { return s.stddev(); });
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
  return list_stats(*list_array, "compute_list_stats");
}

nlohmann::ordered_json compute_fixed_size_list_stats(
  const std::shared_ptr<arrow::FixedSizeListArray> &fixed_list_array)
{
  return list_stats(*fixed_list_array, "compute_fixed_size_list_stats");
}

}  // namespace trossen::io::backends
