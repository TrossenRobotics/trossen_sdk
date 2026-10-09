/**
 * @file lerobot_episode.cpp
 * @brief Implementation of the episode vector widths and the LeRobot `features` schema builder.
 */

#include "trossen_sdk/io/backends/lerobot_common/lerobot_episode.hpp"

#include <iostream>
#include <stdexcept>
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

}  // namespace

bool camera_is_depth(const std::string& camera_name, const nlohmann::json& dataset_info) {
  return dataset_info.at("cameras").at(camera_name).at("is_depth_map").get<bool>();
}

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
    const auto& cam = ep.dataset_info.at("cameras").at(camera_name);
    const int h = cam.at("height").get<int>();
    const int w = cam.at("width").get<int>();
    const bool has_audio = cam.value("has_audio", false);
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
      features[obs_key]["info"]["video.use_log"] = utils::DEFAULT_DEPTH_USE_LOG;
      // lerobot reads the stored unit as info["depth_unit"] (no "video." prefix).
      features[obs_key]["info"]["depth_unit"] = "mm";  // raw mono16 depth is millimeters
    } else {
      // video.codec holds the canonical bitstream name, which is what lerobot itself
      // stores (video_utils.py reads it off the stream as codec.canonical_name) and
      // what it maps back to an encoder through VIDEO_CODECS_ALIASES when re-encoding.
      features[obs_key]["info"]["video.codec"] = "av1";
      features[obs_key]["info"]["video.pix_fmt"] = "yuv420p";
    }
  }

  return features;
}

VideoCodecInfo copied_stream_codec(const std::string& format) {
  if (format == "h264") return {"h264", "yuv420p"};
  if (format == "h265") return {"hevc", "gray12le"};
  throw std::invalid_argument("copied_stream_codec: unsupported video format '" + format + "'");
}

void set_video_codec(
  nlohmann::ordered_json& features, const std::string& obs_key, const VideoCodecInfo& codec) {
  if (!features.contains(obs_key)) return;
  features[obs_key]["info"]["video.codec"] = codec.codec;
  features[obs_key]["info"]["video.pix_fmt"] = codec.pix_fmt;
}

}  // namespace trossen::io::backends
