/**
 * @file mobile_base_config.hpp
 * @brief Configuration for a mobile base hardware component
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__MOBILE_BASE_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__MOBILE_BASE_CONFIG_HPP_

#include <string>

#include "nlohmann/json.hpp"

namespace trossen::configuration {

/**
 * @brief Configuration for the robot's mobile base.
 *
 * The @p type field selects which hardware component is created via
 * HardwareRegistry, the same way @c CameraConfig::type does. Singular on
 * purpose: a robot has one base.
 *
 * JSON format (SLATE — differential drive, no lift):
 * @code
 * {
 *   "type": "slate_base",
 *   "reset_odometry": false,
 *   "enable_torque": false
 * }
 * @endcode
 *
 * JSON format (Rivet — holonomic swerve with a vertical lift):
 * @code
 * {
 *   "type": "trossen_base",
 *   "max_linear_mps": 0.6,
 *   "max_angular_rps": 1.2,
 *   "max_lift_units_per_s": 8000.0,
 *   "ready_timeout_s": 60.0,
 *   "home_on_configure": true,
 *   "command_timeout_ms": 500.0
 * }
 * @endcode
 *
 * Only the two SLATE keys are modelled as fields, because they are the two the
 * Python bindings expose. Everything else rides in @p extra and reaches the
 * component verbatim, so each base keeps its own defaults in one place — its
 * own configure() — rather than having them restated here to drift apart. See
 * `TrossenBaseComponent` for what the Rivet keys mean.
 */
struct MobileBaseConfig {
  /// @brief Hardware registry key — "slate_base" or "trossen_base"
  std::string type{"slate_base"};

  /// @brief Reset base odometry on startup (SLATE only)
  bool reset_odometry{false};

  /// @brief Enable torque on startup (SLATE only)
  bool enable_torque{false};

  /// @brief Passthrough JSON for base-specific settings not modelled above.
  /// Merged into to_json() output so HardwareComponent::configure() receives
  /// them transparently.
  nlohmann::json extra{};

  static MobileBaseConfig from_json(const nlohmann::json& j) {
    MobileBaseConfig c;
    if (j.contains("type")) j.at("type").get_to(c.type);
    if (j.contains("reset_odometry")) j.at("reset_odometry").get_to(c.reset_odometry);
    if (j.contains("enable_torque")) j.at("enable_torque").get_to(c.enable_torque);

    // Parse and erase, so a key added to the struct above cannot also be
    // duplicated into the passthrough — the failure mode a hand-maintained
    // known-keys list invites.
    c.extra = j;
    c.extra.erase("type");
    c.extra.erase("reset_odometry");
    c.extra.erase("enable_torque");
    return c;
  }

  /// @brief Produce JSON suitable for HardwareComponent::configure()
  nlohmann::json to_json() const {
    nlohmann::json j{
      {"reset_odometry", reset_odometry},
      {"enable_torque", enable_torque}
    };
    if (!extra.is_null() && extra.is_object()) {
      j.merge_patch(extra);
    }
    return j;
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__MOBILE_BASE_CONFIG_HPP_
