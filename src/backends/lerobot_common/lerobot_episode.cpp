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
  const DatasetSignalOptions& signals) {
  nlohmann::ordered_json features;

  // Joint names follow the lerobot_trossen robot classes: `<side>_joint_<i>`, with the
  // gripper (last joint) named `<side>_left_carriage_joint`. Each name gets a `.pos`, `.vel`
  // or `.eff` suffix for the value it labels.
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

  // The base block, in the order load_aligned_episode() appends it.
  std::vector<std::string> base_names = {"linear_vel", "angular_vel"};
  if (ep.dataset_info.contains("base_velocity_names")) {
    base_names = ep.dataset_info["base_velocity_names"].get<std::vector<std::string>>();
  }
  if (signals.base_lateral_velocity) base_names.push_back("lateral_vel");
  if (signals.base_pose) {
    base_names.push_back("pose_x");
    base_names.push_back("pose_y");
    base_names.push_back("pose_theta");
  }

  // observation.state (followers)
  nlohmann::json obs_names = nlohmann::json::array();
  for (const auto& follower_stream : ep.follower_streams) {
    const auto stems = joint_stems(follower_stream, ep.joints_per_stream);
    for (const auto& s : stems) obs_names.push_back(s + ".pos");
    if (signals.joint_velocity) {
      for (const auto& s : stems) obs_names.push_back(s + ".vel");
    }
    if (signals.joint_effort) {
      for (const auto& s : stems) obs_names.push_back(s + ".eff");
    }
  }
  if (ep.has_mobile_base) {
    for (const auto& n : base_names) obs_names.push_back(n);
  }
  features["observation.state"]["dtype"] = "float32";
  features["observation.state"]["shape"] = nlohmann::json::array({episode_obs_dim(ep, signals)});
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
  features["action"]["shape"] = nlohmann::json::array({episode_action_dim(ep, signals)});
  features["action"]["names"] = action_names;

  // TODO(shantanuparab-tr): add the observation.images.<camera> video features (#309).

  return features;
}

}  // namespace trossen::io::backends
