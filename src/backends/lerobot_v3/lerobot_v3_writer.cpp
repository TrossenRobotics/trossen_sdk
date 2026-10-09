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

using v3::update_chunk_file_indices;

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
