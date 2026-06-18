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
 *   "actuated": false,                       // optional, default true
 *   "joint_signs":   [1,1,1,-1,-1,1,1],      // optional, default identity
 *   "joint_offsets": [0,0,0,0,0,-0.7854,0]   // optional, default identity
 * }
 */
struct ArmConfig {
  /// @brief Network IP address of the arm controller
  std::string ip_address{"192.168.1.2"};

  /// @brief Robot model identifier (e.g. "wxai_v0")
  std::string model{"wxai_v0"};

  /// @brief End effector type (e.g. "wxai_v0_follower", "wxai_v0_leader")
  std::string end_effector{"wxai_v0_follower"};

  /// @brief Whether the arm has actuators. A passive leader (e.g. the
  /// lightweight Trossen leader) only streams joint positions and cannot be
  /// commanded, so teleop staging / mode setup / rest moves are skipped.
  bool actuated{true};

  /// @brief Optional affine joint remap applied to this arm's read positions:
  /// out[j] = joint_signs[j] * raw[j] + joint_offsets[j]. Empty = identity.
  /// Used for a leader whose joint frame doesn't map 1:1 onto the follower.
  std::vector<float> joint_signs{};
  std::vector<float> joint_offsets{};

  static ArmConfig from_json(const nlohmann::json& j) {
    ArmConfig c;
    if (j.contains("ip_address")) j.at("ip_address").get_to(c.ip_address);
    if (j.contains("model")) j.at("model").get_to(c.model);
    if (j.contains("end_effector")) j.at("end_effector").get_to(c.end_effector);
    if (j.contains("actuated")) j.at("actuated").get_to(c.actuated);
    if (j.contains("joint_signs")) j.at("joint_signs").get_to(c.joint_signs);
    if (j.contains("joint_offsets")) j.at("joint_offsets").get_to(c.joint_offsets);
    return c;
  }

  nlohmann::json to_json() const {
    nlohmann::json j{
      {"ip_address", ip_address},
      {"model", model},
      {"end_effector", end_effector},
      {"actuated", actuated}
    };
    // Emit the remap only when set, to keep ordinary arm configs clean.
    if (!joint_signs.empty()) j["joint_signs"] = joint_signs;
    if (!joint_offsets.empty()) j["joint_offsets"] = joint_offsets;
    return j;
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__ARM_CONFIG_HPP_
