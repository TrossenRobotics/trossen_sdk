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

LeRobotV3DatasetWriter::LeRobotV3DatasetWriter(Options opts) : opts_(std::move(opts)) {}

LeRobotV3DatasetWriter::~LeRobotV3DatasetWriter() {
  close_data_writer();
}

bool LeRobotV3DatasetWriter::open() {
  // TODO(shantanuparab-tr): create the dataset directory tree.
  return false;
}

std::shared_ptr<arrow::Schema> LeRobotV3DatasetWriter::make_data_schema() const {
  // TODO(shantanuparab-tr): build the arrow schema for a v3.0 data file.
  return nullptr;
}

std::shared_ptr<arrow::Table> LeRobotV3DatasetWriter::build_episode_table(
  const AlignedEpisode& ep, int episode_index, int task_index, int64_t global_from) const
{
  // TODO(shantanuparab-tr): build one episode's arrow table.
  return nullptr;
}

bool LeRobotV3DatasetWriter::open_data_writer(const std::shared_ptr<arrow::Schema>& schema) {
  // TODO(shantanuparab-tr): open a new size-rolled parquet data file.
  return false;
}

void LeRobotV3DatasetWriter::close_data_writer() {
  // TODO(shantanuparab-tr): close the open parquet data file.
}

bool LeRobotV3DatasetWriter::roll_data_file_if_needed(int64_t next_ep_frames) {
  // TODO(shantanuparab-tr): roll to the next data file when the size budget is crossed.
  return false;
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

bool LeRobotV3DatasetWriter::finalize() {
  // TODO(shantanuparab-tr): write every metadata file once all episodes are in.
  return false;
}

}  // namespace trossen::io::backends
