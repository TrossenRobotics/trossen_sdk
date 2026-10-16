/**
 * @file mcap_dataset_loader.cpp
 * @brief Implementation of the TrossenMCAP read + alignment path.
 *
 * This translation unit owns the single MCAP_IMPLEMENTATION definition for the SDK;
 * anything linking trossen_sdk must not define the macro.
 */

#define MCAP_IMPLEMENTATION
#include "mcap/reader.hpp"

#include "trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp"

#include <type_traits>

namespace trossen::io::backends {

static_assert(std::is_same_v<McapChannelId, mcap::ChannelId>,
              "McapChannelId must match the MCAP library's channel id type");

bool load_aligned_episode(
  [[maybe_unused]] const std::string& mcap_file,
  [[maybe_unused]] int episode_index,
  [[maybe_unused]] AlignedEpisode& out,
  [[maybe_unused]] McapChannelMap& channels,
  [[maybe_unused]] const DatasetSignalOptions& signals,
  [[maybe_unused]] const AlignmentOptions& alignment)
{
  // TODO(shantanuparab-tr): implement the MCAP decode, leader/follower detection and
  // nearest-timestamp alignment.
  return false;
}

}  // namespace trossen::io::backends
