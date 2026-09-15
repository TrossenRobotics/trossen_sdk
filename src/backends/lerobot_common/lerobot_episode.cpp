/**
 * @file lerobot_episode.cpp
 * @brief Implementation of the LeRobot `features` schema builder.
 */

#include "trossen_sdk/io/backends/lerobot_common/lerobot_episode.hpp"

#include <string>
#include <vector>

namespace trossen::io::backends {

nlohmann::ordered_json build_features(
  const AlignedEpisode& ep,
  bool native_schema,
  const DatasetSignalOptions& signals) {
  // TODO(shantanuparab-tr): implement the LeRobot `features` schema construction.
  return nlohmann::ordered_json::object();
}

}  // namespace trossen::io::backends
