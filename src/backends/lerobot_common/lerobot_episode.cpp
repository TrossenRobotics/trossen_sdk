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
  nlohmann::ordered_json features;

  int joints_per_stream = ep.joints_per_stream > 0 ? ep.joints_per_stream : 7;

  // Joint names come from dataset_info when present, else positional fallbacks.
  auto get_joint_names = [&](const std::string& stream_id, int n) -> nlohmann::json {
    // Native lerobot_trossen naming: `<side>_joint_<i>.pos`, with the gripper
    // (last joint) named `<side>_left_carriage_joint.pos`.
    if (native_schema) {
      nlohmann::json names = nlohmann::json::array();
      const std::string side = stream_side(stream_id);
      const std::string prefix = side.empty() ? "" : side + "_";
      for (int i = 0; i < n; ++i) {
        names.push_back(i == n - 1 ? prefix + "left_carriage_joint.pos"
                                   : prefix + "joint_" + std::to_string(i) + ".pos");
      }
      return names;
    }
    if (!ep.mcap_dataset_info.empty() && ep.mcap_dataset_info.contains("streams") &&
        ep.mcap_dataset_info["streams"].contains(stream_id) &&
        ep.mcap_dataset_info["streams"][stream_id].contains("joint_names")) {
      return ep.mcap_dataset_info["streams"][stream_id]["joint_names"];
    }
    nlohmann::json names = nlohmann::json::array();
    std::string arm_name = stream_id;
    size_t underscore_pos = arm_name.find('_');
    if (underscore_pos != std::string::npos) {
      arm_name = arm_name.substr(underscore_pos + 1);
    }
    for (int i = 0; i < n; ++i) {
      names.push_back(arm_name + "_joint_" + std::to_string(i));
    }
    return names;
  };

  std::vector<std::string> base_vel_names = {"linear_vel", "angular_vel"};
  if (!ep.mcap_dataset_info.empty() && ep.mcap_dataset_info.contains("base_velocity_names")) {
    base_vel_names = ep.mcap_dataset_info["base_velocity_names"].get<std::vector<std::string>>();
  }

  // The base block, in the order load_aligned_episode() appends it.
  std::vector<std::string> base_names = base_vel_names;
  if (signals.base_lateral_velocity) base_names.push_back("lateral_vel");
  if (signals.base_pose) {
    base_names.push_back("pose_x");
    base_names.push_back("pose_y");
    base_names.push_back("pose_theta");
  }

  // Velocity and effort columns reuse the position column names with the suffix swapped,
  // matching the `.pos` / `.vel` / `.eff` naming the lerobot_trossen robot classes use.
  auto suffixed = [](const std::string& pos_name, const char* suffix) {
    const std::string stem =
      pos_name.size() > 4 && pos_name.compare(pos_name.size() - 4, 4, ".pos") == 0
        ? pos_name.substr(0, pos_name.size() - 4)
        : pos_name;
    return stem + suffix;
  };

  // observation.state (followers)
  nlohmann::json obs_names = nlohmann::json::array();
  for (const auto& follower_stream : ep.follower_streams) {
    const nlohmann::json pos_names = get_joint_names(follower_stream, joints_per_stream);
    for (const auto& n : pos_names) obs_names.push_back(n);
    if (signals.joint_velocity) {
      for (const auto& n : pos_names) obs_names.push_back(suffixed(n.get<std::string>(), ".vel"));
    }
    if (signals.joint_effort) {
      for (const auto& n : pos_names) obs_names.push_back(suffixed(n.get<std::string>(), ".eff"));
    }
  }
  const int obs_blocks_per_stream =
    1 + (signals.joint_velocity ? 1 : 0) + (signals.joint_effort ? 1 : 0);
  int obs_state_dim =
    static_cast<int>(ep.follower_streams.size()) * joints_per_stream * obs_blocks_per_stream;
  if (ep.has_mobile_base) {
    for (const auto& n : base_names) obs_names.push_back(n);
    obs_state_dim += static_cast<int>(base_names.size());
  }
  features["observation.state"]["dtype"] = "float32";
  features["observation.state"]["shape"] = nlohmann::json::array({obs_state_dim});
  features["observation.state"]["names"] = obs_names;

  // action (leaders)
  nlohmann::json action_names = nlohmann::json::array();
  for (const auto& leader_stream : ep.leader_streams) {
    for (const auto& n : get_joint_names(leader_stream, joints_per_stream)) {
      action_names.push_back(n);
    }
  }
  int action_dim = static_cast<int>(ep.leader_streams.size()) * joints_per_stream;
  if (ep.has_mobile_base) {
    for (const auto& n : base_names) action_names.push_back(n);
    action_dim += static_cast<int>(base_names.size());
  }
  features["action"]["dtype"] = "float32";
  features["action"]["shape"] = nlohmann::json::array({action_dim});
  features["action"]["names"] = action_names;

  // observation.images.<camera> video features
  for (const auto& camera : ep.cameras) {
    // The episode already carries the LeRobot key; dataset_info is still keyed by the
    // recording's own camera name.
    const std::string& camera_name = camera.name;
    const std::string& obs_key = camera.obs_key;
    const bool is_depth = native_schema && camera_is_depth(camera_name, ep.mcap_dataset_info);

    features[obs_key]["dtype"] = "video";
    features[obs_key]["names"] = nlohmann::json::array({"height", "width", "channels"});

    int h = 480, w = 640, ch = is_depth ? 1 : 3;
    int fps = 30;
    if (!ep.mcap_dataset_info.empty() && ep.mcap_dataset_info.contains("cameras") &&
        ep.mcap_dataset_info["cameras"].contains(camera_name)) {
      const auto& cam = ep.mcap_dataset_info["cameras"][camera_name];
      h = cam.value("height", h);
      w = cam.value("width", w);
      ch = cam.value("channels", ch);
      fps = cam.value("fps", fps);
      features[obs_key]["info"]["has_audio"] = cam.value("has_audio", false);
    } else {
      features[obs_key]["info"]["has_audio"] = false;
    }
    features[obs_key]["shape"] = nlohmann::json::array({h, w, ch});
    features[obs_key]["info"]["video.fps"] = fps;
    features[obs_key]["info"]["video.height"] = h;
    features[obs_key]["info"]["video.width"] = w;
    features[obs_key]["info"]["video.channels"] = ch;
    // lerobot reads this unprefixed key to pick out depth features; the `video.` prefixed
    // spelling it also accepts is its legacy name and is not written.
    features[obs_key]["info"]["is_depth_map"] = is_depth;

    if (is_depth) {
      // Match lerobot 0.6.0 DepthEncoderConfig: HEVC Main 12 / gray12le, 12-bit log-quant.
      features[obs_key]["info"]["video.codec"] = "hevc";
      features[obs_key]["info"]["video.pix_fmt"] = "gray12le";
      features[obs_key]["info"]["video.depth_min"] = 0.01;   // meters, quantum 0
      features[obs_key]["info"]["video.depth_max"] = 10.0;   // meters, quantum DEPTH_QMAX
      features[obs_key]["info"]["video.shift"] = 3.5;        // meters, pre-log offset
      features[obs_key]["info"]["video.use_log"] = true;
      // lerobot reads the stored unit as info["depth_unit"] (no "video." prefix).
      features[obs_key]["info"]["depth_unit"] = "mm";  // raw mono16 depth is millimeters
    } else {
      // video.codec holds the canonical bitstream name, which is what lerobot itself
      // stores (video_utils.py reads it off the stream as codec.canonical_name) and
      // what it maps back to an encoder through VIDEO_CODECS_ALIASES when re-encoding.
      // Overridden per camera by the writer for a remuxed stream.
      features[obs_key]["info"]["video.codec"] = "av1";
      features[obs_key]["info"]["video.pix_fmt"] = "yuv420p";
    }
  }

  return features;
}

}  // namespace trossen::io::backends
