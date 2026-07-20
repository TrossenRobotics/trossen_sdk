/**
 * @file rivet_config.hpp
 * @brief Configuration for a bimanual follower pair (RivetComponent)
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__RIVET_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__RIVET_CONFIG_HPP_

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace trossen::configuration {

/**
 * @brief Configuration for a pair of follower arms (RivetComponent)
 *
 * JSON format:
 * {
 *   "left_ip_address": "192.168.1.5",
 *   "left_model": "pro",
 *   "right_ip_address": "192.168.1.4",
 *   "right_model": "pro",
 *   "end_effector": "wxai_v0_follower",
 *   "write_moving_time_s": 0.2,          // optional, default 0.0
 *   "staging_time_s": 2.0,               // optional, default 2.0
 *   "staged_position": [...],            // optional, default none (no staging)
 *   "episode_lifecycle_enabled": true,   // optional, default false
 *   "position_min": [...], "position_max": [...],
 *   "velocity_max": [...], "effort_max": [...],
 *   "position_tolerance": [...], "velocity_tolerance": [...], "effort_tolerance": [...]
 * }
 */
struct RivetConfig {
  /// @brief Network IP address of the left arm's controller
  std::string left_ip_address{"192.168.1.5"};

  /// @brief Left arm model identifier (e.g. "pro")
  std::string left_model{"pro"};

  /// @brief Network IP address of the right arm's controller
  std::string right_ip_address{"192.168.1.4"};

  /// @brief Right arm model identifier (e.g. "pro")
  std::string right_model{"pro"};

  /// @brief End effector type shared by both arms (e.g. "wxai_v0_follower")
  std::string end_effector{"wxai_v0_follower"};

  /// @brief Per-tick trajectory time (seconds) passed to set_all_positions in
  /// write_joint(). Zero applies the goal immediately.
  float write_moving_time_s{0.0f};

  /// @brief Staging time (seconds): duration of the point-to-point moves in
  /// stage() and the end_teleop() rest move.
  float staging_time_s{2.0f};

  /// @brief Joint-space pose both arms move to at session start (via stage()).
  /// Empty = no staging.
  std::vector<float> staged_position{};

  /// @brief Whether this pair participates in the SessionManager's per-episode
  /// lifecycle. Opt-in; defaults to false.
  bool episode_lifecycle_enabled{false};

  /// @brief Optional per-joint operating limits pushed to both controllers on
  /// connect. Each array, when non-empty, must have one entry per joint. Empty
  /// = leave the controller's firmware default untouched for that field.
  std::vector<float> position_min{};
  std::vector<float> position_max{};
  std::vector<float> velocity_max{};
  std::vector<float> effort_max{};

  /// @brief Optional per-joint limit tolerances pushed alongside the limits
  /// above. Same emptiness semantics as the limits.
  std::vector<float> position_tolerance{};
  std::vector<float> velocity_tolerance{};
  std::vector<float> effort_tolerance{};

  static RivetConfig from_json(const nlohmann::json& j) {
    RivetConfig c;
    if (j.contains("left_ip_address")) j.at("left_ip_address").get_to(c.left_ip_address);
    if (j.contains("left_model")) j.at("left_model").get_to(c.left_model);
    if (j.contains("right_ip_address")) j.at("right_ip_address").get_to(c.right_ip_address);
    if (j.contains("right_model")) j.at("right_model").get_to(c.right_model);
    if (j.contains("end_effector")) j.at("end_effector").get_to(c.end_effector);
    if (j.contains("write_moving_time_s")) {
      j.at("write_moving_time_s").get_to(c.write_moving_time_s);
    }
    if (j.contains("staging_time_s")) j.at("staging_time_s").get_to(c.staging_time_s);
    if (j.contains("staged_position")) j.at("staged_position").get_to(c.staged_position);
    if (j.contains("episode_lifecycle_enabled")) {
      j.at("episode_lifecycle_enabled").get_to(c.episode_lifecycle_enabled);
    }
    if (j.contains("position_min")) j.at("position_min").get_to(c.position_min);
    if (j.contains("position_max")) j.at("position_max").get_to(c.position_max);
    if (j.contains("velocity_max")) j.at("velocity_max").get_to(c.velocity_max);
    if (j.contains("effort_max")) j.at("effort_max").get_to(c.effort_max);
    if (j.contains("position_tolerance")) {
      j.at("position_tolerance").get_to(c.position_tolerance);
    }
    if (j.contains("velocity_tolerance")) {
      j.at("velocity_tolerance").get_to(c.velocity_tolerance);
    }
    if (j.contains("effort_tolerance")) {
      j.at("effort_tolerance").get_to(c.effort_tolerance);
    }
    return c;
  }

  nlohmann::json to_json() const {
    nlohmann::json j{
      {"left_ip_address", left_ip_address},
      {"left_model", left_model},
      {"right_ip_address", right_ip_address},
      {"right_model", right_model},
      {"end_effector", end_effector},
      {"write_moving_time_s", write_moving_time_s},
      {"staging_time_s", staging_time_s},
      {"episode_lifecycle_enabled", episode_lifecycle_enabled}
    };
    // Emit staging only when configured, to keep ordinary configs clean.
    if (!staged_position.empty()) j["staged_position"] = staged_position;
    // Emit per-joint limits only when set.
    if (!position_min.empty()) j["position_min"] = position_min;
    if (!position_max.empty()) j["position_max"] = position_max;
    if (!velocity_max.empty()) j["velocity_max"] = velocity_max;
    if (!effort_max.empty()) j["effort_max"] = effort_max;
    // Emit per-joint tolerances only when set.
    if (!position_tolerance.empty()) j["position_tolerance"] = position_tolerance;
    if (!velocity_tolerance.empty()) j["velocity_tolerance"] = velocity_tolerance;
    if (!effort_tolerance.empty()) j["effort_tolerance"] = effort_tolerance;
    return j;
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__RIVET_CONFIG_HPP_
