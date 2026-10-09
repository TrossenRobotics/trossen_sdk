/**
 * @file lerobot_v3_writer.cpp
 * @brief Implementation of the LeRobot v3.0 aggregating dataset writer.
 */

#include "trossen_sdk/io/backends/lerobot_v3/lerobot_v3_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>

#include <arrow/io/api.h>
#include <opencv2/opencv.hpp>
#include <parquet/arrow/writer.h>

#include "trossen_sdk/io/backends/lerobot_v3/lerobot_v3_constants.hpp"
#include "trossen_sdk/utils/depth_quantization.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_schema_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_stats_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_readme_utils.hpp"

namespace trossen::io::backends {

namespace fs = std::filesystem;
namespace v3 = trossen::io::backends::lerobot_v3;

namespace {

constexpr size_t kMaxImageSamplesPerKey = 1000;  // cap pixel-stat sampling per camera
const std::vector<double> kQuantiles = {0.01, 0.10, 0.50, 0.90, 0.99};
const std::vector<std::string> kQuantileKeys = {"q01", "q10", "q50", "q90", "q99"};

using v3::update_chunk_file_indices;

/// @brief True if the directory holds any `.png` frame (depth cams; RGB cams use `.jpg`).
///
/// @param dir Directory of extracted frames to scan; must exist.
/// @return true if at least one regular `.png` file is present, false otherwise.
bool dir_has_png(const fs::path& dir) {
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".png") return true;
  }
  return false;
}

/// @brief Linear-interpolated quantile of a sorted sample (numpy's default method).
///
/// @param sorted Sample values in ascending order; every caller sorts before calling.
/// @param q Quantile to take, in [0, 1].
/// @return The interpolated value, or 0 for an empty sample.
float quantile_of(const std::vector<float>& sorted, double q) {
  if (sorted.empty()) return 0.0f;
  if (sorted.size() == 1) return sorted[0];
  double pos = q * (static_cast<double>(sorted.size()) - 1.0);
  size_t lo = static_cast<size_t>(std::floor(pos));
  size_t hi = static_cast<size_t>(std::ceil(pos));
  double frac = pos - static_cast<double>(lo);
  return static_cast<float>(sorted[lo] * (1.0 - frac) + sorted[hi] * frac);
}

/// @brief Per-dimension stats for a vector feature → nested JSON (lists of length D).
///
/// @param per_dim One value column per feature dimension, accumulated across every episode.
/// @param count Row count to report as `count`; the caller's dataset total, not a column length.
/// @return JSON holding min, max, mean, std and one entry per key in kQuantileKeys, each a
///         list of length D.
nlohmann::ordered_json vector_stats(const std::vector<std::vector<float>>& per_dim,
                                    int64_t count) {
  nlohmann::ordered_json out;
  nlohmann::json mn = nlohmann::json::array(), mx = nlohmann::json::array(),
                 me = nlohmann::json::array(), sd = nlohmann::json::array();
  std::vector<nlohmann::json> q(kQuantiles.size());
  for (auto& qa : q) qa = nlohmann::json::array();

  for (const auto& col : per_dim) {
    double sum = 0.0, sumsq = 0.0;
    float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
    for (float v : col) {
      sum += v;
      sumsq += static_cast<double>(v) * v;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    double n = col.empty() ? 1.0 : static_cast<double>(col.size());
    double mean = sum / n;
    double var = std::max(0.0, sumsq / n - mean * mean);
    mn.push_back(col.empty() ? 0.0f : lo);
    mx.push_back(col.empty() ? 0.0f : hi);
    me.push_back(static_cast<float>(mean));
    sd.push_back(static_cast<float>(std::sqrt(var)));

    std::vector<float> sorted = col;
    std::sort(sorted.begin(), sorted.end());
    for (size_t k = 0; k < kQuantiles.size(); ++k) {
      q[k].push_back(quantile_of(sorted, kQuantiles[k]));
    }
  }

  out["min"] = mn;
  out["max"] = mx;
  out["mean"] = me;
  out["std"] = sd;
  out["count"] = nlohmann::json::array({count});
  for (size_t k = 0; k < kQuantiles.size(); ++k) out[kQuantileKeys[k]] = q[k];
  return out;
}

/// @brief Wrap three per-channel scalars as a LeRobot image-stat tensor of shape [3,1,1].
///
/// @param rgb One value per channel, in RGB order.
/// @return Nested JSON array shaped [3][1][1].
nlohmann::json channels_to_chw(const std::array<float, 3>& rgb) {
  nlohmann::json t = nlohmann::json::array();
  for (int c = 0; c < 3; ++c) t.push_back(nlohmann::json::array({nlohmann::json::array({rgb[c]})}));
  return t;
}

/// @brief Per-channel (RGB, normalized to [0,1]) stats over sampled images → [3,1,1] JSON.
///
/// @param images Samples from sample_images(): CV_32FC3, BGR, already scaled to [0,1].
/// @param count Number of images the stats were computed over (LeRobot's `count` for image
///        features is the sample count, not the episode frame count).
/// @return JSON holding min, max, mean, std and one entry per key in kQuantileKeys, each a
///         [3,1,1] tensor.
nlohmann::ordered_json image_stats(const std::vector<cv::Mat>& images, int64_t count) {
  // Collect per-channel pixel values (RGB order) across all samples.
  std::array<std::vector<float>, 3> chan;
  for (const auto& img : images) {
    if (img.empty()) continue;
    // sample_images() hands back float32 BGR in [0,1]. Reading these as 8-bit would
    // reinterpret the mantissa bytes as pixels and yield uniform-noise statistics.
    if (img.type() != CV_32FC3) {
      std::cerr << "Warning: skipping image sample with unexpected type " << img.type()
                << " (expected CV_32FC3)\n";
      continue;
    }
    for (int y = 0; y < img.rows; ++y) {
      const cv::Vec3f* row = img.ptr<cv::Vec3f>(y);
      for (int x = 0; x < img.cols; ++x) {
        // OpenCV is BGR; store as RGB.
        chan[0].push_back(row[x][2]);
        chan[1].push_back(row[x][1]);
        chan[2].push_back(row[x][0]);
      }
    }
  }

  std::array<float, 3> mn{}, mx{}, me{}, sd{};
  std::array<std::array<float, 3>, 5> q{};  // [quantile][channel]
  for (int c = 0; c < 3; ++c) {
    auto& col = chan[c];
    if (col.empty()) continue;
    double sum = 0.0, sumsq = 0.0;
    float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
    for (float v : col) {
      sum += v;
      sumsq += static_cast<double>(v) * v;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    double n = static_cast<double>(col.size());
    double mean = sum / n;
    double var = std::max(0.0, sumsq / n - mean * mean);
    mn[c] = lo;
    mx[c] = hi;
    me[c] = static_cast<float>(mean);
    sd[c] = static_cast<float>(std::sqrt(var));
    std::sort(col.begin(), col.end());
    for (size_t k = 0; k < kQuantiles.size(); ++k) q[k][c] = quantile_of(col, kQuantiles[k]);
  }

  nlohmann::ordered_json out;
  out["min"] = channels_to_chw(mn);
  out["max"] = channels_to_chw(mx);
  out["mean"] = channels_to_chw(me);
  out["std"] = channels_to_chw(sd);
  out["count"] = nlohmann::json::array({count});
  for (size_t k = 0; k < kQuantiles.size(); ++k) out[kQuantileKeys[k]] = channels_to_chw(q[k]);
  return out;
}

}  // namespace

LeRobotV3DatasetWriter::LeRobotV3DatasetWriter(Options opts) : opts_(std::move(opts)) {}

LeRobotV3DatasetWriter::~LeRobotV3DatasetWriter() {
  close_data_writer();
}

bool LeRobotV3DatasetWriter::open() {
  meta_dir_ = opts_.dataset_root / v3::META_DIR;
  data_dir_ = opts_.dataset_root / v3::DATA_DIR;
  videos_dir_ = opts_.dataset_root / v3::VIDEO_DIR;
  try {
    fs::create_directories(meta_dir_ / "episodes");
    fs::create_directories(data_dir_);
    fs::create_directories(videos_dir_);
  } catch (const std::exception& e) {
    std::cerr << "Error: Failed to create dataset directories: " << e.what() << "\n";
    return false;
  }
  return true;
}

std::shared_ptr<arrow::Schema> LeRobotV3DatasetWriter::make_data_schema() const {
  return arrow::schema({
    arrow::field("action", arrow::fixed_size_list(arrow::float32(), action_dim_)),
    arrow::field("observation.state", arrow::fixed_size_list(arrow::float32(), obs_dim_)),
    arrow::field("timestamp", arrow::float32()),
    arrow::field("frame_index", arrow::int64()),
    arrow::field("episode_index", arrow::int64()),
    arrow::field("index", arrow::int64()),
    arrow::field("task_index", arrow::int64()),
  });
}

std::shared_ptr<arrow::Table> LeRobotV3DatasetWriter::build_episode_table(
  const AlignedEpisode& ep, int episode_index, int task_index, int64_t global_from) const
{
  arrow::FloatBuilder ts_b;
  auto obs_vb = std::make_shared<arrow::FloatBuilder>();
  arrow::FixedSizeListBuilder obs_b(arrow::default_memory_pool(), obs_vb, obs_dim_);
  auto act_vb = std::make_shared<arrow::FloatBuilder>();
  arrow::FixedSizeListBuilder act_b(arrow::default_memory_pool(), act_vb, action_dim_);
  arrow::Int64Builder frame_b, epi_b, idx_b, task_b;
  auto* obs_val = static_cast<arrow::FloatBuilder*>(obs_b.value_builder());
  auto* act_val = static_cast<arrow::FloatBuilder*>(act_b.value_builder());

  for (size_t i = 0; i < ep.frames.size(); ++i) {
    const auto& f = ep.frames[i];
    (void)ts_b.Append(f.timestamp_s);
    (void)obs_b.Append();
    for (double v : f.observation) (void)obs_val->Append(static_cast<float>(v));
    (void)act_b.Append();
    for (double v : f.action) (void)act_val->Append(static_cast<float>(v));
    (void)frame_b.Append(static_cast<int64_t>(i));
    (void)epi_b.Append(episode_index);
    (void)idx_b.Append(global_from + static_cast<int64_t>(i));
    (void)task_b.Append(task_index);
  }

  std::shared_ptr<arrow::Array> ts_a, obs_a, act_a, frame_a, epi_a, idx_a, task_a;
  (void)ts_b.Finish(&ts_a);
  (void)obs_b.Finish(&obs_a);
  (void)act_b.Finish(&act_a);
  (void)frame_b.Finish(&frame_a);
  (void)epi_b.Finish(&epi_a);
  (void)idx_b.Finish(&idx_a);
  (void)task_b.Finish(&task_a);

  return arrow::Table::Make(data_schema_,
                            {act_a, obs_a, ts_a, frame_a, epi_a, idx_a, task_a});
}

bool LeRobotV3DatasetWriter::open_data_writer(const std::shared_ptr<arrow::Schema>& schema) {
  std::ostringstream rel;
  rel << "chunk-" << std::setfill('0') << std::setw(3) << data_.chunk_index << "/file-"
      << std::setfill('0') << std::setw(3) << data_.file_index << ".parquet";
  data_.path = data_dir_ / rel.str();
  try {
    fs::create_directories(data_.path.parent_path());
  } catch (const std::exception& e) {
    std::cerr << "Error: Failed to create data chunk dir: " << e.what() << "\n";
    return false;
  }

  auto out_res = arrow::io::FileOutputStream::Open(data_.path.string());
  if (!out_res.ok()) {
    std::cerr << "Error: Failed to open data parquet: " << data_.path << "\n";
    return false;
  }
  data_.out = *out_res;
  auto props =
    parquet::WriterProperties::Builder().compression(parquet::Compression::SNAPPY)->build();
  auto arrow_props = parquet::ArrowWriterProperties::Builder().store_schema()->build();
  auto wr_res = parquet::arrow::FileWriter::Open(*schema, arrow::default_memory_pool(), data_.out,
                                                 props, arrow_props);
  if (!wr_res.ok()) {
    std::cerr << "Error: Failed to open parquet writer: " << wr_res.status().ToString() << "\n";
    return false;
  }
  data_.writer = std::move(wr_res).ValueUnsafe();
  data_.frames_in_file = 0;
  return true;
}

void LeRobotV3DatasetWriter::close_data_writer() {
  if (data_.writer) {
    (void)data_.writer->Close();
    data_.writer.reset();
  }
  if (data_.out) {
    (void)data_.out->Close();
    data_.out.reset();
  }
}

bool LeRobotV3DatasetWriter::roll_data_file_if_needed(int64_t next_ep_frames) {
  if (!data_.writer) {
    return open_data_writer(data_schema_);
  }
  if (data_.frames_in_file == 0) return true;

  // Estimate bytes/frame from the fixed schema and project whether the next episode
  // would push the current file past the size budget. (File partitioning only; does
  // not affect dataset correctness.)
  const double bytes_per_frame =
    static_cast<double>(action_dim_ + obs_dim_) * sizeof(float) + 5.0 * sizeof(int64_t);
  const double budget_bytes = static_cast<double>(opts_.data_files_size_in_mb) * 1e6;
  const double projected =
    static_cast<double>(data_.frames_in_file + next_ep_frames) * bytes_per_frame;
  if (projected >= budget_bytes) {
    close_data_writer();
    update_chunk_file_indices(data_.chunk_index, data_.file_index, opts_.chunks_size);
    return open_data_writer(data_schema_);
  }
  return true;
}

bool LeRobotV3DatasetWriter::encode_episode_video(
  const fs::path& image_dir, size_t frame_count, const fs::path& out_mp4) const
{
  // TODO(shantanuparab-tr): encode an episode's color frames to video.
  return false;
}

bool LeRobotV3DatasetWriter::encode_depth_video(
  const fs::path& image_dir, size_t frame_count, const fs::path& out_mp4) const
{
  // TODO(shantanuparab-tr): encode an episode's depth frames to video.
  return false;
}

bool LeRobotV3DatasetWriter::remux_episode_video(
  const fs::path& annexb, size_t frame_count, const fs::path& out_mp4) const
{
  // TODO(shantanuparab-tr): remux an already-compressed stream without re-encoding.
  return false;
}

std::vector<cv::Mat> LeRobotV3DatasetWriter::sample_video_frames(
  const fs::path& mp4, size_t frame_count, const fs::path& tmp_dir) const
{
  // TODO(shantanuparab-tr): sample frames back out of a written video for pixel statistics.
  return {};
}

bool LeRobotV3DatasetWriter::place_or_concat_video(
  const std::string& video_key, const fs::path& episode_mp4, double ep_duration_s,
  std::array<double, 4>& out_slot)
{
  // TODO(shantanuparab-tr): place the episode video, concatenating into the current file when it
  // fits.
  return false;
}

int LeRobotV3DatasetWriter::task_index_for(const std::string& task_name) {
  // TODO(shantanuparab-tr): map a task name to its row in the tasks table.
  return 0;
}

LeRobotV3DatasetWriter::PreparedEpisode LeRobotV3DatasetWriter::prepare_episode(
  const fs::path& mcap_path,
  int episode_index,
  const std::string& fallback_task,
  const fs::path& tmp_root) const
{
  // TODO(shantanuparab-tr): decode, extract and encode one episode on a worker thread.
  return {};
}

bool LeRobotV3DatasetWriter::consume_episode(PreparedEpisode& pe)
{
  // TODO(shantanuparab-tr): append a prepared episode to the aggregated files, in order.
  return false;
}

bool LeRobotV3DatasetWriter::write_episodes_parquet() {
  // TODO(shantanuparab-tr): write the per-episode metadata table.
  return false;
}

bool LeRobotV3DatasetWriter::write_tasks_parquet() {
  // TODO(shantanuparab-tr): write the tasks table.
  return false;
}

bool LeRobotV3DatasetWriter::write_stats_json() {
  // TODO(shantanuparab-tr): write the aggregated dataset statistics.
  return false;
}

bool LeRobotV3DatasetWriter::write_info_json() {
  // TODO(shantanuparab-tr): write the v3.0 info.json.
  return false;
}

bool LeRobotV3DatasetWriter::write_readme() {
  // TODO(shantanuparab-tr): write the dataset README.
  return false;
}

bool LeRobotV3DatasetWriter::finalize() {
  // TODO(shantanuparab-tr): write every metadata file once all episodes are in.
  return false;
}

}  // namespace trossen::io::backends
