/**
 * @file sdk_config.cpp
 * @brief Implementation of SdkConfig and sub-config from_json methods
 */

#include "trossen_sdk/configuration/sdk_config.hpp"

#include "trossen_sdk/configuration/global_config.hpp"

namespace trossen::configuration {

// --- HardwareConfig ----------------------------------------------------------

HardwareConfig HardwareConfig::from_json(const nlohmann::json& j) {
  HardwareConfig c;

  if (j.contains("arms") && j.at("arms").is_object()) {
    for (const auto& [id, arm_j] : j.at("arms").items()) {
      c.arms[id] = ArmConfig::from_json(arm_j);
    }
  }

  if (j.contains("cameras") && j.at("cameras").is_array()) {
    for (const auto& cam_j : j.at("cameras")) {
      c.cameras.push_back(CameraConfig::from_json(cam_j));
    }
  }

  if (j.contains("mobile_base") && j.at("mobile_base").is_object()) {
    c.mobile_base = MobileBaseConfig::from_json(j.at("mobile_base"));
  }

  return c;
}

// ─── SdkConfig ───────────────────────────────────────────────────────────────

SdkConfig SdkConfig::from_json(const nlohmann::json& j) {
  SdkConfig c;

  if (j.contains("robot_name")) j.at("robot_name").get_to(c.robot_name);

  if (j.contains("hardware") && j.at("hardware").is_object()) {
    c.hardware = HardwareConfig::from_json(j.at("hardware"));
  }

  if (j.contains("producers") && j.at("producers").is_array()) {
    for (const auto& p : j.at("producers")) {
      c.producers.push_back(ProducerConfig::from_json(p));
    }
  }

  if (j.contains("teleop") && j.at("teleop").is_object()) {
    c.teleop = TeleoperationConfig::from_json(j.at("teleop"));
  }

  // Parse the TrossenMCAP backend config; seed robot_name from the top-level field
  // so it does not need to be repeated inside the "backend" section.
  c.mcap_backend.robot_name = c.robot_name;
  if (j.contains("backend") && j.at("backend").is_object()) {
    c.mcap_backend = TrossenMCAPBackendConfig::from_json(j.at("backend"));
    if (!j.at("backend").contains("robot_name")) {
      c.mcap_backend.robot_name = c.robot_name;
    }
  }

  if (j.contains("session") && j.at("session").is_object()) {
    c.session = SessionManagerConfig::from_json(j.at("session"));
  }

  return c;
}

void SdkConfig::populate_global_config() const {
  nlohmann::json gc_json = nlohmann::json::object();

  // Session manager
  {
    nlohmann::json sm;
    sm["type"] = "session_manager";
    if (session.max_duration.has_value()) {
      sm["max_duration"] = session.max_duration->count();
    }
    if (session.max_episodes.has_value()) {
      sm["max_episodes"] = session.max_episodes.value();
    }
    sm["backend_type"] = session.backend_type;
    if (session.reset_duration.has_value()) {
      sm["reset_duration"] = session.reset_duration->count();
    }
    gc_json["session_manager"] = sm;
  }

  // TrossenMCAP backend
  {
    const auto& b = mcap_backend;
    nlohmann::json bj;
    bj["type"] = "trossen_mcap_backend";
    bj["root"] = b.root;
    bj["robot_name"] = b.robot_name;
    bj["chunk_size_bytes"] = b.chunk_size_bytes;
    bj["compression"] = b.compression;
    bj["dataset_id"] = b.dataset_id;
    bj["episode_index"] = b.episode_index;
    gc_json["trossen_mcap_backend"] = bj;
  }

  // Teleop (consumed by trossen::hw::teleop::create_controllers_from_global_config)
  {
    nlohmann::json tj;
    tj["type"] = "teleop";
    tj["enabled"] = teleop.enabled;
    tj["rate_hz"] = teleop.rate_hz;
    nlohmann::json pairs_j = nlohmann::json::array();
    for (const auto& p : teleop.pairs) {
      pairs_j.push_back({
        {"leader",   p.leader},
        {"follower", p.follower},
        {"space",    p.space},
      });
    }
    tj["pairs"] = pairs_j;
    gc_json["teleop"] = tj;
  }

  GlobalConfig::instance().load_from_json(gc_json);
}

}  // namespace trossen::configuration
