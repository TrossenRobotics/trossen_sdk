/**
 * @file arm_config.hpp
 * @brief Configuration for robot arm components
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__ROBOTS__COMPONENTS__ARM_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__ROBOTS__COMPONENTS__ARM_CONFIG_HPP_

#include <string>
#include <nlohmann/json.hpp>

namespace trossen::configuration {

/**
 * @brief Configuration for a single robot arm
 *
 * Supports various arm models including WidowX AI and SO-101
 */
struct ArmConfig {
  /// Arm model (e.g., "wxai_v0", "vxai_v0_left", "vxai_v0_right", "so101")
  std::string model{"wxai_v0"};

  /// IP address for network-connected arms (WidowX)
  std::string ip_address{"192.168.1.2"};

  /// End effector type (e.g., "wxai_v0_leader", "wxai_v0_follower", "wxai_v0_base")
  std::string end_effector{"wxai_v0_follower"};

  /// Whether to clear errors on startup
  bool clear_error{false};

  /// Joint update rate in Hz
  float joint_rate_hz{200.0f};

  /// Optional identifier for this arm
  std::string id{""};

  /**
   * @brief Create ArmConfig from JSON
   * @param j JSON object containing arm configuration
   * @return Populated ArmConfig with defaults for missing fields
   */
  static ArmConfig from_json(const nlohmann::json& j) {
    ArmConfig c;

    if (j.contains("model")) j.at("model").get_to(c.model);
    if (j.contains("ip_address")) j.at("ip_address").get_to(c.ip_address);
    if (j.contains("end_effector")) j.at("end_effector").get_to(c.end_effector);
    if (j.contains("clear_error")) j.at("clear_error").get_to(c.clear_error);
    if (j.contains("joint_rate_hz")) j.at("joint_rate_hz").get_to(c.joint_rate_hz);
    if (j.contains("id")) j.at("id").get_to(c.id);

    return c;
  }

  /**
   * @brief Convert ArmConfig to JSON
   * @return JSON representation of this configuration
   */
  nlohmann::json to_json() const {
    nlohmann::json j;
    j["model"] = model;
    j["ip_address"] = ip_address;
    j["end_effector"] = end_effector;
    j["clear_error"] = clear_error;
    j["joint_rate_hz"] = joint_rate_hz;
    if (!id.empty()) j["id"] = id;
    return j;
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__ROBOTS__COMPONENTS__ARM_CONFIG_HPP_
