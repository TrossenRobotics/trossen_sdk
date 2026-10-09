/**
 * @file lerobot_episode.cpp
 * @brief Implementation of the episode vector widths and the LeRobot `features` schema builder.
 */

#include "trossen_sdk/io/backends/lerobot_common/lerobot_episode.hpp"

#include <string>
#include <vector>

namespace trossen::io::backends {

int base_block_width(const DatasetSignalOptions& signals) {
  return 2 + (signals.base_lateral_velocity ? 1 : 0) + (signals.base_pose ? 3 : 0);
}

int episode_action_dim(const AlignedEpisode& ep, const DatasetSignalOptions& signals) {
  int dim = static_cast<int>(ep.leader_streams.size()) * ep.joints_per_stream;
  if (ep.has_mobile_base) dim += base_block_width(signals);
  return dim;
}

int episode_obs_dim(const AlignedEpisode& ep, const DatasetSignalOptions& signals) {
  const int blocks_per_stream =
    1 + (signals.joint_velocity ? 1 : 0) + (signals.joint_effort ? 1 : 0);
  int dim =
    static_cast<int>(ep.follower_streams.size()) * ep.joints_per_stream * blocks_per_stream;
  if (ep.has_mobile_base) dim += base_block_width(signals);
  return dim;
}

nlohmann::ordered_json build_features(
  const AlignedEpisode& ep,
  bool native_schema,
  const DatasetSignalOptions& signals) {
  // TODO(shantanuparab-tr): implement the LeRobot `features` schema construction.
  return nlohmann::ordered_json::object();
}

}  // namespace trossen::io::backends
