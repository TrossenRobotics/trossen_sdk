/**
 * @file arm_config.hpp
 * @brief Configuration for a robot arm hardware component
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__ARM_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__ARM_CONFIG_HPP_

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace trossen::configuration {

/**
 * @brief Configuration for a single robot arm (TrossenArmComponent)
 *
 * JSON format:
 * {
 *   "ip_address": "192.168.1.3",
 *   "model": "wxai_v0",
 *   "end_effector": "wxai_v0_leader",
 *   "initial_position": [0, 0.0, 0.0, 0.0, 0, 0, 0],  // optional, joint-space
 *   "initial_position_time_s": 2.0                    // optional, default 2.0
 * }
 */
struct ArmConfig {
  /// @brief Network IP address of the arm controller
  std::string ip_address{"192.168.1.2"};

  /// @brief Robot model identifier (e.g. "wxai_v0")
  std::string model{"wxai_v0"};

  /// @brief End effector type (e.g. "wxai_v0_follower", "wxai_v0_leader")
  std::string end_effector{"wxai_v0_follower"};

  /// @brief Joint-space pose to move to before each episode starts.
  /// Empty (default) disables this behavior.
  std::vector<float> initial_position{};

  /// @brief Trajectory time (seconds) used to reach `initial_position`.
  float initial_position_time_s{2.0f};

  static ArmConfig from_json(const nlohmann::json& j) {
    ArmConfig c;
    if (j.contains("ip_address")) j.at("ip_address").get_to(c.ip_address);
    if (j.contains("model")) j.at("model").get_to(c.model);
    if (j.contains("end_effector")) j.at("end_effector").get_to(c.end_effector);
    if (j.contains("initial_position")) {
      j.at("initial_position").get_to(c.initial_position);
    }
    if (j.contains("initial_position_time_s")) {
      j.at("initial_position_time_s").get_to(c.initial_position_time_s);
    }
    return c;
  }

  nlohmann::json to_json() const {
    nlohmann::json j{
      {"ip_address", ip_address},
      {"model", model},
      {"end_effector", end_effector},
      {"initial_position_time_s", initial_position_time_s}
    };
    if (!initial_position.empty()) {
      j["initial_position"] = initial_position;
    }
    return j;
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__ARM_CONFIG_HPP_
