/**
 * @file teleop_config.hpp
 * @brief Configuration for teleoperation setup
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__TELEOP_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__TELEOP_CONFIG_HPP_

#include <cmath>
#include <cstddef>
#include <stdexcept>
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
  /// @brief Id of the leader component in the hardware registry. Required.
  std::string leader;

  /// @brief Id of the follower component in the hardware registry. Empty runs the pair
  /// leader-only.
  std::string follower;

  /// @brief Teleop space: "joint" or "cartesian". Defaults to "joint".
  ///
  /// The factory converts this to teleop::TeleopCapable::Space. The chosen
  /// space must be implemented (via JointSpaceTeleop / CartesianSpaceTeleop)
  /// by both the leader and the follower — otherwise the controller throws
  /// at construction.
  std::string space{"joint"};

  /// @brief Parses one pair.
  /// @param j The pair object from "teleop.pairs".
  /// @return The parsed pair.
  /// @throws std::invalid_argument if "leader" is missing or empty.
  static TeleoperationPair from_json(const nlohmann::json& j) {
    TeleoperationPair p;
    if (j.contains("leader")) j.at("leader").get_to(p.leader);
    if (j.contains("follower")) j.at("follower").get_to(p.follower);
    if (j.contains("space")) j.at("space").get_to(p.space);
    if (p.leader.empty()) {
      throw std::invalid_argument(
        "'leader' is required; set it to the id of the component the follower mirrors");
    }
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
 *     { "leader": "leader_left",  "follower": "follower_left",  "space": "joint" },
 *     { "leader": "leader_right", "follower": "follower_right", "space": "joint" }
 *   ]
 * }
 *
 * The teleop loop mirrors each leader's state to its paired follower at the
 * specified rate. Set "enabled" to false to skip teleop (e.g. replay mode).
 * The "space" field on each pair selects the teleop space ("joint" or
 * "cartesian"); it defaults to "joint" if omitted.
 *
 * Per-arm tuning (staging pose, trajectory time, etc.) lives in each arm's
 * own hardware configuration block.
 */
struct TeleoperationConfig : public BaseConfig {
  /// @brief Whether teleoperation is active
  bool enabled{true};

  /// @brief Teleoperation control loop rate in Hz
  float rate_hz{1000.0f};

  /// @brief Leader->follower pairings
  std::vector<TeleoperationPair> pairs;

  std::string type() const override { return "teleop"; }

  /// @brief Parses the "teleop" block.
  /// @param j The "teleop" object.
  /// @return The parsed config.
  /// @throws std::invalid_argument naming "teleop.rate_hz" if the rate is not positive and
  ///   finite, or "teleop.pairs[i]" if a pair is invalid.
  static TeleoperationConfig from_json(const nlohmann::json& j) {
    TeleoperationConfig c;
    if (j.contains("enabled")) j.at("enabled").get_to(c.enabled);
    if (j.contains("rate_hz")) j.at("rate_hz").get_to(c.rate_hz);
    if (!(c.rate_hz > 0.0f) || !std::isfinite(c.rate_hz)) {
      throw std::invalid_argument(
        "teleop.rate_hz: must be positive and finite; set it to the mirror rate in Hz");
    }
    if (j.contains("pairs")) {
      const auto& pairs_j = j.at("pairs");
      for (std::size_t i = 0; i < pairs_j.size(); ++i) {
        try {
          c.pairs.push_back(TeleoperationPair::from_json(pairs_j.at(i)));
        } catch (const std::invalid_argument& e) {
          throw std::invalid_argument(
            "teleop.pairs[" + std::to_string(i) + "]: " + e.what());
        }
      }
    }
    return c;
  }
};

REGISTER_CONFIG(TeleoperationConfig, "teleop");

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__TELEOP_CONFIG_HPP_
