/**
 * @file trossen_mobile_ai_config.hpp
 * @brief Configuration for Trossen Mobile AI (2 leaders + 2 followers + 1 base)
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__ROBOTS__TROSSEN_MOBILE_AI_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__ROBOTS__TROSSEN_MOBILE_AI_CONFIG_HPP_

#include <optional>
#include <vector>

#include "trossen_sdk/configuration/base_config.hpp"
#include "trossen_sdk/configuration/config_registry.hpp"
#include "trossen_sdk/configuration/types/robots/components/arm_config.hpp"
#include "trossen_sdk/configuration/types/robots/components/camera_config.hpp"
#include "trossen_sdk/configuration/types/robots/components/mobile_base_config.hpp"

namespace trossen::configuration {

/**
 * @brief Configuration for Trossen Mobile AI robot system
 *
 * Trossen Mobile AI consists of:
 * - 2 leader arms (for bimanual teleoperation control)
 * - 2 follower arms (mimic leader movements)
 * - 1 mobile base (for navigation and positioning)
 * - Multiple cameras for observation
 */
struct TrossenMobileAiConfig : public BaseConfig {
  /// Robot system name/identifier
  std::string robot_name{"trossen_mobile_ai"};

  /// Arm configurations (should have exactly 4: 2 leaders + 2 followers)
  std::vector<ArmConfig> arms;

  /// Camera configurations
  std::vector<CameraConfig> cameras;

  /// Mobile base configuration (optional but typically 1)
  std::optional<MobileBaseConfig> mobile_base;

  std::string type() const override { return "trossen_mobile_ai"; }

  /**
   * @brief Create TrossenMobileAiConfig from JSON
   * @param j JSON object containing robot configuration with nested components
   * @return Populated TrossenMobileAiConfig
   */
  static TrossenMobileAiConfig from_json(const nlohmann::json& j) {
    TrossenMobileAiConfig c;

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

    // Parse mobile base configuration
    if (j.contains("mobile_base")) {
      c.mobile_base = MobileBaseConfig::from_json(j["mobile_base"]);
    }

    return c;
  }

  /**
   * @brief Convert TrossenMobileAiConfig to JSON
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

    // Serialize mobile base
    if (mobile_base.has_value()) {
      j["mobile_base"] = mobile_base.value().to_json();
    }

    return j;
  }
};

REGISTER_CONFIG(TrossenMobileAiConfig, "trossen_mobile_ai");

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__ROBOTS__TROSSEN_MOBILE_AI_CONFIG_HPP_
