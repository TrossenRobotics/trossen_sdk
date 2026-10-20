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

/**
 * @brief Check whether a camera is a depth map.
 *
 * The camera's boolean `is_depth_map` flag in dataset_info decides when present. A `_depth`
 * name suffix is used only for a camera that has no such flag.
 *
 * @param camera_name Camera id as recorded, e.g. `cam_high_depth`.
 * @param dataset_info The recording's dataset_info metadata; may be empty.
 * @return True if the camera records depth, false if it records color.
 */
bool camera_is_depth(const std::string& camera_name, const nlohmann::json& dataset_info) {
  if (dataset_info.contains("cameras") && dataset_info["cameras"].contains(camera_name)) {
    const auto& camera = dataset_info["cameras"][camera_name];
    if (auto flag = camera.find("is_depth_map"); flag != camera.end() && flag->is_boolean()) {
      return flag->get<bool>();
    }
  }
  constexpr std::string_view kSuffix = "_depth";
  return camera_name.ends_with(kSuffix);
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
  // TODO(shantanuparab-tr): implement the LeRobot `features` schema construction.
  return nlohmann::ordered_json::object();
}

}  // namespace trossen::io::backends
