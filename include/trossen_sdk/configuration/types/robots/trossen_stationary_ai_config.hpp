/**
 * @file trossen_stationary_ai_config.hpp
 * @brief Configuration for Trossen Stationary AI (2 leaders + 2 followers)
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__ROBOTS__TROSSEN_STATIONARY_AI_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__ROBOTS__TROSSEN_STATIONARY_AI_CONFIG_HPP_

#include <vector>

#include "trossen_sdk/configuration/base_config.hpp"
#include "trossen_sdk/configuration/config_registry.hpp"
#include "trossen_sdk/configuration/types/robots/components/arm_config.hpp"
#include "trossen_sdk/configuration/types/robots/components/camera_config.hpp"

namespace trossen::configuration {

/**
 * @brief Configuration for Trossen Stationary AI robot system
 *
 * Trossen Stationary AI consists of:
 * - 2 leader arms (for bimanual teleoperation control)
 * - 2 follower arms (mimic leader movements)
 * - Multiple cameras for observation
 * - Fixed base (stationary setup)
 */
struct TrossenStationaryAiConfig : public BaseConfig {
  /// Robot system name/identifier
  std::string robot_name{"trossen_stationary_ai"};

  /// Arm configurations (should have exactly 4: 2 leaders + 2 followers)
  std::vector<ArmConfig> arms;

  /// Camera configurations
  std::vector<CameraConfig> cameras;

  std::string type() const override { return "trossen_stationary_ai"; }

  /**
   * @brief Create TrossenStationaryAiConfig from JSON
   * @param j JSON object containing robot configuration with nested components
   * @return Populated TrossenStationaryAiConfig
   */
  static TrossenStationaryAiConfig from_json(const nlohmann::json& j) {
    TrossenStationaryAiConfig c;

    if (j.contains("robot_name")) {
      j.at("robot_name").get_to(c.robot_name);
    }

    // Parse nested arm configurations
    if (j.contains("arms") && j["arms"].is_array()) {
      for (const auto& arm_json : j["arms"]) {
        c.arms.push_back(ArmConfig::from_json(arm_json));
      }
    }

    // Parse nested camera configurations
    if (j.contains("cameras") && j["cameras"].is_array()) {
      for (const auto& cam_json : j["cameras"]) {
        c.cameras.push_back(CameraConfig::from_json(cam_json));
      }
    }

    return c;
  }

  /**
   * @brief Convert TrossenStationaryAiConfig to JSON
   * @return JSON representation with nested components
   */
  nlohmann::json to_json() const {
    nlohmann::json j;
    j["type"] = type();
    j["robot_name"] = robot_name;

    // Serialize arms
    nlohmann::json arms_json = nlohmann::json::array();
    for (const auto& arm : arms) {
      arms_json.push_back(arm.to_json());
    }
    j["arms"] = arms_json;

    // Serialize cameras
    nlohmann::json cameras_json = nlohmann::json::array();
    for (const auto& camera : cameras) {
      cameras_json.push_back(camera.to_json());
    }
    j["cameras"] = cameras_json;

    return j;
  }
};

REGISTER_CONFIG(TrossenStationaryAiConfig, "trossen_stationary_ai");

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__ROBOTS__TROSSEN_STATIONARY_AI_CONFIG_HPP_
