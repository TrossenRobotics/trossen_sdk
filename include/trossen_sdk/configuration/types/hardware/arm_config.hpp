/**
 * @file arm_config.hpp
 * @brief Configuration for a robot arm hardware component
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__ARM_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__ARM_CONFIG_HPP_

#include <string>

#include "nlohmann/json.hpp"

namespace trossen::configuration {

/**
 * @brief Configuration for a single robot arm (TrossenArmComponent)
 *
 * JSON format:
 * {
 *   "ip_address": "192.168.1.3",
 *   "model": "wxai_v0",
 *   "end_effector": "wxai_v0_leader"
 * }
 */
struct ArmConfig {
  /// @brief Network IP address of the arm controller
  std::string ip_address{"192.168.1.2"};

  /// @brief Robot model identifier (e.g. "wxai_v0")
  std::string model{"wxai_v0"};

  /// @brief End effector type (e.g. "wxai_v0_follower", "wxai_v0_leader")
  std::string end_effector{"wxai_v0_follower"};

  static ArmConfig from_json(const nlohmann::json& j) {
    ArmConfig c;
    if (j.contains("ip_address")) j.at("ip_address").get_to(c.ip_address);
    if (j.contains("model")) j.at("model").get_to(c.model);
    if (j.contains("end_effector")) j.at("end_effector").get_to(c.end_effector);
    return c;
  }

  nlohmann::json to_json() const {
    return nlohmann::json{
      {"ip_address", ip_address},
      {"model", model},
      {"end_effector", end_effector}
    };
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__ARM_CONFIG_HPP_
