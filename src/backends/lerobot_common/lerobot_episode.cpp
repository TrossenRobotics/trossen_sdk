/**
 * @file lerobot_episode.cpp
 * @brief Implementation of the LeRobot `features` schema builder.
 */

#include "trossen_sdk/io/backends/lerobot_common/lerobot_episode.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace trossen::io::backends {

namespace {

/// @brief Strip the `follower_`/`leader_` prefix from a stream id, leaving the side name.
///
/// A stream id carrying neither prefix is reported and used whole, so its joint columns
/// are still named after something traceable back to the recording.
std::string stream_side(const std::string& stream_id) {
  for (std::string_view p : {"follower_", "leader_"}) {
    if (stream_id.rfind(p, 0) == 0) return stream_id.substr(p.size());
  }
  std::cerr << "Warning: stream '" << stream_id << "' has no 'follower_' or 'leader_' prefix; "
            << "naming its joint columns after the full stream id\n";
  return stream_id;
}

/// @brief True if a camera is a depth map (dataset_info flag or a `_depth` name suffix).
bool camera_is_depth(const std::string& camera_name, const nlohmann::json& dataset_info) {
  if (!dataset_info.empty() && dataset_info.contains("cameras") &&
      dataset_info["cameras"].contains(camera_name) &&
      dataset_info["cameras"][camera_name].value("is_depth_map", false)) {
    return true;
  }
  constexpr std::string_view kSuffix = "_depth";
  return camera_name.size() >= kSuffix.size() &&
         camera_name.compare(camera_name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

}  // namespace

nlohmann::ordered_json build_features(
  const AlignedEpisode& ep,
  bool native_schema,
  const DatasetSignalOptions& signals) {
  // TODO(shantanuparab-tr): implement the LeRobot `features` schema construction.
  return nlohmann::ordered_json::object();
}

}  // namespace trossen::io::backends
