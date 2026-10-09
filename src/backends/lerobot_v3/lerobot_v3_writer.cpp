/**
 * @file lerobot_v3_writer.cpp
 * @brief Implementation of the LeRobot v3.0 aggregating dataset writer.
 */

#include "trossen_sdk/io/backends/lerobot_v3/lerobot_v3_writer.hpp"

#include <utility>

namespace trossen::io::backends {

namespace fs = std::filesystem;

LeRobotV3DatasetWriter::LeRobotV3DatasetWriter(Options opts) : opts_(std::move(opts)) {}

LeRobotV3DatasetWriter::~LeRobotV3DatasetWriter() {
  close_data_writer();
}

bool LeRobotV3DatasetWriter::open() {
  // TODO(shantanuparab-tr): create the dataset directory tree (#345).
  return false;
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
