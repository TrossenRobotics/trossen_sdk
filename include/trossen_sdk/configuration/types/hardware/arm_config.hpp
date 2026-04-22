/**
 * @file arm_config.hpp
 * @brief Configuration for a robot arm hardware component
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__ARM_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__ARM_CONFIG_HPP_

#include <optional>
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
 *   "staged_position": [0.0, 1.05, 0.52, 0.63, 0.0, 0.0, 0.0],
 *   "teleop_moving_time_s": 2.0
 * }
 *
 * `staged_position` and `teleop_moving_time_s` are optional — the
 * component treats an empty staged_position as "no staging" and falls
 * back to its internal default moving time when the field is absent.
 */
struct ArmConfig {
  /// @brief Network IP address of the arm controller
  std::string ip_address{"192.168.1.2"};

  /// @brief Robot model identifier (e.g. "wxai_v0")
  std::string model{"wxai_v0"};

  /// @brief End effector type (e.g. "wxai_v0_follower", "wxai_v0_leader")
  std::string end_effector{"wxai_v0_follower"};

  /// @brief Joint-space pose the arm moves to via stage() / restage().
  /// Empty means staging is disabled for this arm.
  std::vector<float> staged_position;

  /// @brief Trajectory time (seconds) for stage() and end_teleop() moves.
  /// Nullopt lets the component fall back to its internal default.
  std::optional<float> teleop_moving_time_s;

  static ArmConfig from_json(const nlohmann::json& j) {
    ArmConfig c;
    if (j.contains("ip_address"))  j.at("ip_address").get_to(c.ip_address);
    if (j.contains("model"))        j.at("model").get_to(c.model);
    if (j.contains("end_effector")) j.at("end_effector").get_to(c.end_effector);
    if (j.contains("staged_position")) {
      j.at("staged_position").get_to(c.staged_position);
    }
    if (j.contains("teleop_moving_time_s")) {
      c.teleop_moving_time_s = j.at("teleop_moving_time_s").get<float>();
    }
    return c;
  }

  nlohmann::json to_json() const {
    nlohmann::json j{
      {"ip_address",   ip_address},
      {"model",        model},
      {"end_effector", end_effector}
    };
    // Only emit optional fields when they carry a value, so the output
    // round-trips to the same shape that was read in.
    if (!staged_position.empty()) {
      j["staged_position"] = staged_position;
    }
    if (teleop_moving_time_s.has_value()) {
      j["teleop_moving_time_s"] = *teleop_moving_time_s;
    }
    return j;
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__ARM_CONFIG_HPP_
