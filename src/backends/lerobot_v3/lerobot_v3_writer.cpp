/**
 * @file lerobot_v3_writer.cpp
 * @brief Implementation of the LeRobot v3.0 aggregating dataset writer.
 */

#include "trossen_sdk/io/backends/lerobot_v3/lerobot_v3_writer.hpp"

#include <cstdint>
#include <iostream>
#include <system_error>
#include <utility>

namespace trossen::io::backends {

namespace fs = std::filesystem;
namespace v3 = trossen::io::backends::lerobot_v3;

namespace {

/// @brief Per-episode metadata directory under meta/.
constexpr char kEpisodesDir[] = "episodes";

}  // namespace

LeRobotV3DatasetWriter::LeRobotV3DatasetWriter(Options opts) : opts_(std::move(opts)) {}

LeRobotV3DatasetWriter::~LeRobotV3DatasetWriter() {
  close_data_writer();
}

bool LeRobotV3DatasetWriter::open() {
  if (opts_.chunks_size <= 0) {
    std::cerr << "Error: chunks_size must be positive, got " << opts_.chunks_size << "\n";
    return false;
  }
  // A size budget of 0 starts a new file for every episode.
  const std::pair<const char*, int> budgets[] = {
    {"data_files_size_in_mb", opts_.data_files_size_in_mb},
    {"video_files_size_in_mb", opts_.video_files_size_in_mb},
  };
  for (const auto& [name, value] : budgets) {
    if (value < 0) {
      std::cerr << "Error: " << name << " must not be negative, got " << value << "\n";
      return false;
    }
  }

  meta_dir_ = opts_.dataset_root / v3::META_DIR;
  data_dir_ = opts_.dataset_root / v3::DATA_DIR;
  videos_dir_ = opts_.dataset_root / v3::VIDEO_DIR;
  for (const fs::path& dir : {meta_dir_ / kEpisodesDir, data_dir_, videos_dir_}) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
      std::cerr << "Error: Failed to create " << dir << ": " << ec.message() << "\n";
      return false;
    }
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

  arrow::Status st;
  for (size_t i = 0; i < ep.frames.size(); ++i) {
    const auto& f = ep.frames[i];
    if (f.observation.size() != static_cast<size_t>(obs_dim_) ||
        f.action.size() != static_cast<size_t>(action_dim_)) {
      std::cerr << "Error: Row " << i << " has " << f.observation.size() << " observation and "
                << f.action.size() << " action values; the dataset has " << obs_dim_ << " and "
                << action_dim_ << "\n";
      return nullptr;
    }
    st &= ts_b.Append(f.timestamp_s);
    st &= obs_b.Append();
    for (double v : f.observation) st &= obs_val->Append(static_cast<float>(v));
    st &= act_b.Append();
    for (double v : f.action) st &= act_val->Append(static_cast<float>(v));
    st &= frame_b.Append(static_cast<int64_t>(i));
    st &= epi_b.Append(episode_index);
    st &= idx_b.Append(global_from + static_cast<int64_t>(i));
    st &= task_b.Append(task_index);
  }

  std::shared_ptr<arrow::Array> ts_a, obs_a, act_a, frame_a, epi_a, idx_a, task_a;
  st &= ts_b.Finish(&ts_a);
  st &= obs_b.Finish(&obs_a);
  st &= act_b.Finish(&act_a);
  st &= frame_b.Finish(&frame_a);
  st &= epi_b.Finish(&epi_a);
  st &= idx_b.Finish(&idx_a);
  st &= task_b.Finish(&task_a);
  if (!st.ok()) {
    std::cerr << "Error: Failed to build episode table: " << st.ToString() << "\n";
    return nullptr;
  }

  return arrow::Table::Make(data_schema_,
                            {act_a, obs_a, ts_a, frame_a, epi_a, idx_a, task_a});
}

bool LeRobotV3DatasetWriter::close_data_writer() {
  // TODO(shantanuparab-tr): close the open parquet data file (#345).
  return true;
}

bool LeRobotV3DatasetWriter::encode_episode_video(
  const fs::path& image_dir, size_t frame_count, const fs::path& out_mp4) const
{
  // TODO(shantanuparab-tr): encode an episode's color frames to video (#354).
  return false;
}

bool LeRobotV3DatasetWriter::encode_depth_video(
  const fs::path& image_dir, size_t frame_count, const fs::path& out_mp4) const
{
  // TODO(shantanuparab-tr): encode an episode's depth frames to video (#354).
  return false;
}

bool LeRobotV3DatasetWriter::remux_episode_video(
  const fs::path& annexb, size_t frame_count, const fs::path& out_mp4) const
{
  // TODO(shantanuparab-tr): remux an already-compressed stream without re-encoding (#354).
  return false;
}

std::vector<cv::Mat> LeRobotV3DatasetWriter::sample_video_frames(
  const fs::path& mp4, size_t frame_count, const fs::path& tmp_dir) const
{
  // TODO(shantanuparab-tr): sample frames back out of a written video for pixel statistics (#355).
  return {};
}

bool LeRobotV3DatasetWriter::place_or_concat_video(
  const std::string& video_key, const fs::path& episode_mp4, double ep_duration_s,
  std::array<double, 4>& out_slot)
{
  // TODO(shantanuparab-tr): place the episode video, concatenating into the current file when it
  // fits (#355).
  return false;
}

int LeRobotV3DatasetWriter::task_index_for(const std::string& task_name) {
  // TODO(shantanuparab-tr): map a task name to its row in the tasks table (#365).
  return 0;
}

LeRobotV3DatasetWriter::PreparedEpisode LeRobotV3DatasetWriter::prepare_episode(
  const fs::path& mcap_path,
  int episode_index,
  const std::string& fallback_task,
  const fs::path& tmp_root) const
{
  // TODO(shantanuparab-tr): decode, extract and encode one episode on a worker thread (#364).
  return {};
}

bool LeRobotV3DatasetWriter::consume_episode(PreparedEpisode& pe)
{
  // TODO(shantanuparab-tr): append a prepared episode to the aggregated files, in order (#365).
  return false;
}

bool LeRobotV3DatasetWriter::write_episodes_parquet() {
  // TODO(shantanuparab-tr): write the per-episode metadata table (#374).
  return false;
}

bool LeRobotV3DatasetWriter::write_tasks_parquet() {
  // TODO(shantanuparab-tr): write the tasks table (#374).
  return false;
}

bool LeRobotV3DatasetWriter::write_stats_json() {
  // TODO(shantanuparab-tr): write the aggregated dataset statistics (#375).
  return false;
}

bool LeRobotV3DatasetWriter::write_info_json() {
  // TODO(shantanuparab-tr): write the v3.0 info.json (#375).
  return false;
}

bool LeRobotV3DatasetWriter::write_readme() {
  // TODO(shantanuparab-tr): write the dataset README (#375).
  return false;
}

bool LeRobotV3DatasetWriter::finalize() {
  // TODO(shantanuparab-tr): write every metadata file once all episodes are in (#375).
  return false;
}

}  // namespace trossen::io::backends
