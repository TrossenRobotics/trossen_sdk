/**
 * @file mobile_base_config.hpp
 * @brief Configuration for a mobile base hardware component
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__MOBILE_BASE_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__MOBILE_BASE_CONFIG_HPP_

#include "nlohmann/json.hpp"

namespace trossen::configuration {

/**
 * @brief Configuration for the SLATE mobile base (SlateBaseComponent)
 *
 * JSON format:
 * {
 *   "reset_odometry": false,
 *   "enable_torque": false
 * }
 */
struct MobileBaseConfig {
  /// @brief Reset base odometry on startup
  bool reset_odometry{false};

  /// @brief Enable torque on startup
  bool enable_torque{false};

  static MobileBaseConfig from_json(const nlohmann::json& j) {
    MobileBaseConfig c;
    if (j.contains("reset_odometry")) j.at("reset_odometry").get_to(c.reset_odometry);
    if (j.contains("enable_torque")) j.at("enable_torque").get_to(c.enable_torque);
    return c;
  }

  nlohmann::json to_json() const {
    return nlohmann::json{
      {"reset_odometry", reset_odometry},
      {"enable_torque", enable_torque}
    };
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__MOBILE_BASE_CONFIG_HPP_
