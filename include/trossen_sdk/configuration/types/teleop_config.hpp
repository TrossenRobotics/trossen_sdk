/**
 * @file teleop_config.hpp
 * @brief Configuration for teleoperation setup
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__TELEOP_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__TELEOP_CONFIG_HPP_

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/base_config.hpp"
#include "trossen_sdk/configuration/config_registry.hpp"

namespace trossen::configuration {

/**
 * @brief A single leader->follower arm pairing for teleoperation
 */
struct TeleoperationPair {
  /// @brief Key in hardware.arms map for the leader arm
  std::string leader;

  /// @brief Key in hardware.arms map for the follower arm
  std::string follower;

  static TeleoperationPair from_json(const nlohmann::json& j) {
    TeleoperationPair p;
    if (j.contains("leader")) j.at("leader").get_to(p.leader);
    if (j.contains("follower")) j.at("follower").get_to(p.follower);
    return p;
  }
};

/**
 * @brief Teleoperation configuration
 *
 * JSON format (under "teleop"):
 * {
 *   "enabled": true,
 *   "rate_hz": 1000.0,
 *   "pairs": [
 *     { "leader": "leader_left",  "follower": "follower_left"  },
 *     { "leader": "leader_right", "follower": "follower_right" }
 *   ]
 * }
 *
 * The teleop loop mirrors each leader arm's joint positions to its paired follower
 * at the specified rate. Set "enabled" to false to skip teleop (e.g. replay mode).
 */
struct TeleoperationConfig : public BaseConfig {
  /// @brief Whether teleoperation is active
  bool enabled{true};

  /// @brief Teleoperation control loop rate in Hz
  float rate_hz{1000.0f};

  /// @brief Leader->follower pairings
  std::vector<TeleoperationPair> pairs;

  std::string type() const override { return "teleop"; }

  static TeleoperationConfig from_json(const nlohmann::json& j) {
    TeleoperationConfig c;
    if (j.contains("enabled")) j.at("enabled").get_to(c.enabled);
    if (j.contains("rate_hz")) j.at("rate_hz").get_to(c.rate_hz);
    if (j.contains("pairs")) {
      for (const auto& pair_j : j.at("pairs")) {
        c.pairs.push_back(TeleoperationPair::from_json(pair_j));
      }
    }
    return c;
  }
};

REGISTER_CONFIG(TeleoperationConfig, "teleop");

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__TELEOP_CONFIG_HPP_
