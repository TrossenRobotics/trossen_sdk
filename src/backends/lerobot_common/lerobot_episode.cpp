/**
 * @file lerobot_episode.cpp
 * @brief Implementation of the episode vector widths and the LeRobot `features` schema builder.
 */

#include "trossen_sdk/io/backends/lerobot_common/lerobot_episode.hpp"

namespace trossen::io::backends {

int episode_action_dim(const AlignedEpisode& ep) {
  int dim = static_cast<int>(ep.leader_streams.size()) * ep.joints_per_stream;
  if (ep.has_mobile_base) dim += BASE_VELOCITY_WIDTH;
  return dim;
}

int episode_obs_dim(const AlignedEpisode& ep) {
  int dim = static_cast<int>(ep.follower_streams.size()) * ep.joints_per_stream;
  if (ep.has_mobile_base) dim += BASE_VELOCITY_WIDTH;
  return dim;
}

nlohmann::ordered_json build_features(const AlignedEpisode& ep) {
  // TODO(shantanuparab-tr): implement the LeRobot `features` schema construction (#408).
  return nlohmann::ordered_json::object();
}

}  // namespace trossen::io::backends
