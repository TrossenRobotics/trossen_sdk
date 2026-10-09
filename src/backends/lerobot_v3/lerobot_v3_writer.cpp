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

void LeRobotV3DatasetWriter::close_data_writer() {
  // TODO(shantanuparab-tr): close the open parquet data file.
}

bool LeRobotV3DatasetWriter::finalize() {
  // TODO(shantanuparab-tr): write every metadata file once all episodes are in.
  return false;
}

}  // namespace trossen::io::backends
