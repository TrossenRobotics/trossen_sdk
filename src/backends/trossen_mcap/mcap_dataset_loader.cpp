/**
 * @file mcap_dataset_loader.cpp
 * @brief Implementation of the TrossenMCAP read + alignment path.
 *
 * This translation unit owns the single MCAP_IMPLEMENTATION definition for the SDK;
 * anything linking trossen_sdk must not define the macro.
 */

#define MCAP_IMPLEMENTATION
#include "trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <regex>
#include <string_view>

#include "JointState.pb.h"
#include "Odometry2D.pb.h"

namespace trossen::io::backends {

bool load_aligned_episode(
  const std::string& mcap_file,
  int episode_index,
  AlignedEpisode& out,
  McapChannelMap& channels,
  const DatasetSignalOptions& signals,
  const AlignmentOptions& alignment)
{
  // TODO(shantanuparab-tr): implement the MCAP decode, leader/follower detection and
  // nearest-timestamp alignment.
  return false;
}

}  // namespace trossen::io::backends
