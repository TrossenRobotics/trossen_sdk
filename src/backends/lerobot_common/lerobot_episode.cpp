/**
 * @file lerobot_episode.cpp
 * @brief Implementation of the episode vector widths and the LeRobot `features` schema builder.
 */

#include "trossen_sdk/io/backends/lerobot_common/lerobot_episode.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace trossen::io::backends {

namespace {

/**
 * @brief Strip the `follower_`/`leader_` prefix from a stream id, leaving the side name.
 *
 * A bare `follower` or `leader` is a single-arm recording and has no side, so its joint
 * columns take no prefix, as in the single-arm lerobot_trossen robot class. A stream id
 * carrying neither prefix is reported and used whole, so its joint columns are still named
 * after something traceable back to the recording.
 *
 * @param stream_id Arm stream id from the recording, e.g. `follower_left`.
 * @return The side name (`left`), an empty string for a bare `follower` or `leader`, or
 *   @p stream_id unchanged when it has neither prefix.
 */
std::string stream_side(const std::string& stream_id) {
  if (stream_id == "follower" || stream_id == "leader") return "";
  for (std::string_view p : {"follower_", "leader_"}) {
    if (stream_id.starts_with(p)) return stream_id.substr(p.size());
  }
  std::cerr << "Warning: stream '" << stream_id << "' has no 'follower_' or 'leader_' prefix; "
            << "naming its joint columns after the full stream id\n";
  return stream_id;
}

}  // namespace

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
