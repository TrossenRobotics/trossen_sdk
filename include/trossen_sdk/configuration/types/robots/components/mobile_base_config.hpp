/**
 * @file mobile_base_config.hpp
 * @brief Configuration for mobile robot base components
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__ROBOTS__COMPONENTS__MOBILE_BASE_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__ROBOTS__COMPONENTS__MOBILE_BASE_CONFIG_HPP_

#include <string>
#include <nlohmann/json.hpp>

namespace trossen::configuration {

/**
 * @brief Configuration for a mobile robot base
 *
 * Supports various mobile base types for navigation and manipulation
 */
struct MobileBaseConfig {
  /// MobileBase type (e.g., "tracer", "scout", "custom")
  std::string type{"slate"};

  /// Maximum linear velocity in m/s
  float max_linear_velocity{1.0f};

  /// Maximum angular velocity in rad/s
  float max_angular_velocity{2.0f};

  /// Update rate in Hz
  float update_rate{50.0f};

  /// Optional identifier for this mobile base
  std::string id{""};

  /**
   * @brief Create MobileBaseConfig from JSON
   * @param j JSON object containing mobile base configuration
   * @return Populated MobileBaseConfig with defaults for missing fields
   */
  static MobileBaseConfig from_json(const nlohmann::json& j) {
    MobileBaseConfig c;

    if (j.contains("type")) j.at("type").get_to(c.type);
    if (j.contains("max_linear_velocity")) {
      j.at("max_linear_velocity").get_to(c.max_linear_velocity);
    }
    if (j.contains("max_angular_velocity")) {
      j.at("max_angular_velocity").get_to(c.max_angular_velocity);
    }
    if (j.contains("update_rate")) j.at("update_rate").get_to(c.update_rate);
    if (j.contains("id")) j.at("id").get_to(c.id);

    return c;
  }

  /**
   * @brief Convert MobileBaseConfig to JSON
   * @return JSON representation of this configuration
   */
  nlohmann::json to_json() const {
    nlohmann::json j;
    j["type"] = type;
    j["max_linear_velocity"] = max_linear_velocity;
    j["max_angular_velocity"] = max_angular_velocity;
    j["update_rate"] = update_rate;
    if (!id.empty()) j["id"] = id;
    return j;
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__ROBOTS__COMPONENTS__MOBILE_BASE_CONFIG_HPP_
