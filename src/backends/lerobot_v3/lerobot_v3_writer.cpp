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

bool LeRobotV3DatasetWriter::finalize() {
  // TODO(shantanuparab-tr): write every metadata file once all episodes are in (#375).
  return false;
}

}  // namespace trossen::io::backends
