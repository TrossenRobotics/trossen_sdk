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
  nlohmann::ordered_json features;

  // Joint names follow the lerobot_trossen robot classes: `<side>_joint_<i>`, with the
  // gripper (last joint) named `<side>_left_carriage_joint`. Each name gets a `.pos` suffix.
  auto joint_stems = [](const std::string& stream_id, int n) {
    std::vector<std::string> stems;
    const std::string side = stream_side(stream_id);
    const std::string prefix = side.empty() ? "" : side + "_";
    for (int i = 0; i < n; ++i) {
      stems.push_back(i == n - 1 ? prefix + "left_carriage_joint"
                                 : prefix + "joint_" + std::to_string(i));
    }
    return stems;
  };

  // The base block, in the order load_aligned_episode() appends it. Its velocity names are
  // required for a mobile base by load_aligned_episode().
  std::vector<std::string> base_names;
  if (ep.has_mobile_base) {
    base_names = ep.dataset_info.at("mobile_base")
                   .at("velocity_names")
                   .get<std::vector<std::string>>();
  }

  // observation.state (followers)
  nlohmann::json obs_names = nlohmann::json::array();
  for (const auto& follower_stream : ep.follower_streams) {
    for (const auto& s : joint_stems(follower_stream, ep.joints_per_stream)) {
      obs_names.push_back(s + ".pos");
    }
  }
  if (ep.has_mobile_base) {
    for (const auto& n : base_names) obs_names.push_back(n);
  }
  features["observation.state"]["dtype"] = "float32";
  features["observation.state"]["shape"] = nlohmann::json::array({episode_obs_dim(ep)});
  features["observation.state"]["names"] = obs_names;

  // action (leaders)
  nlohmann::json action_names = nlohmann::json::array();
  for (const auto& leader_stream : ep.leader_streams) {
    for (const auto& s : joint_stems(leader_stream, ep.joints_per_stream)) {
      action_names.push_back(s + ".pos");
    }
  }
  if (ep.has_mobile_base) {
    for (const auto& n : base_names) action_names.push_back(n);
  }
  features["action"]["dtype"] = "float32";
  features["action"]["shape"] = nlohmann::json::array({episode_action_dim(ep)});
  features["action"]["names"] = action_names;

  // TODO(shantanuparab-tr): add the observation.images.<camera> video features (#309).

  return features;
}

}  // namespace trossen::io::backends
