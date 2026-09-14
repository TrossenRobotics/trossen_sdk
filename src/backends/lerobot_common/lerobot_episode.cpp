/**
 * @file lerobot_episode.cpp
 * @brief Implementation of the episode vector widths and the LeRobot `features` schema builder.
 */

#include "trossen_sdk/io/backends/lerobot_common/lerobot_episode.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "trossen_sdk/utils/depth_quantization.hpp"

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

  // observation.images.<camera> video features
  for (const auto& camera : ep.cameras) {
    // The episode already carries the LeRobot key; dataset_info is still keyed by the
    // recording's own camera name.
    const std::string& camera_name = camera.name;
    const std::string& obs_key = camera.obs_key;
    const bool is_depth = camera_is_depth(camera_name, ep.dataset_info);

    features[obs_key]["dtype"] = "video";
    features[obs_key]["names"] = nlohmann::json::array({"height", "width", "channels"});

    // Depth is always encoded as 1-channel gray12le and color as 3-channel yuv420p.
    const int ch = is_depth ? 1 : 3;
    int h = DEFAULT_CAMERA_HEIGHT, w = DEFAULT_CAMERA_WIDTH;
    bool has_audio = false;
    if (ep.dataset_info.contains("cameras") && ep.dataset_info["cameras"].contains(camera_name)) {
      const auto& cam = ep.dataset_info["cameras"][camera_name];
      h = cam.value("height", h);
      w = cam.value("width", w);
      has_audio = cam.value("has_audio", false);
    }
    features[obs_key]["info"]["has_audio"] = has_audio;
    features[obs_key]["shape"] = nlohmann::json::array({h, w, ch});
    // Every output video, encoded or remuxed, is written at the dataset rate with one frame
    // per row, so its rate is ep.fps rather than the camera's capture rate.
    features[obs_key]["info"]["video.fps"] = static_cast<int>(ep.fps);
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
      features[obs_key]["info"]["video.depth_min"] = utils::DEFAULT_DEPTH_MIN_M;  // quantum 0
      features[obs_key]["info"]["video.depth_max"] = utils::DEFAULT_DEPTH_MAX_M;  // DEPTH_QMAX
      features[obs_key]["info"]["video.shift"] = utils::DEFAULT_DEPTH_SHIFT_M;  // pre-log offset
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
