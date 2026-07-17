/**
 * @file bimanual_glide_config.hpp
 * @brief Configuration for a bimanual glide leader pair (BimanualGlideComponent)
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__BIMANUAL_GLIDE_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__BIMANUAL_GLIDE_CONFIG_HPP_

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace trossen::configuration {

/**
 * @brief Configuration for a pair of glide leader arms (BimanualGlideComponent)
 *
 * JSON format:
 * {
 *   "left_ip_address": "192.168.1.3",
 *   "left_model": "glide",
 *   "right_ip_address": "192.168.1.2",
 *   "right_model": "glide",
 *   "write_moving_time_s": 0.2,            // optional, default 0.0
 *   "episode_lifecycle_enabled": true,     // optional, default false
 *   "joint_signs":   [1,1,1,-1,-1,1,1],     // optional, default identity
 *   "joint_offsets": [0,0,0,0,0,-0.7854,0], // optional, default identity
 *   "position_min": [...], "position_max": [...],
 *   "velocity_max": [...], "effort_max": [...],
 *   "position_tolerance": [...], "velocity_tolerance": [...], "effort_tolerance": [...],
 *   "gripper_feedback_leader_max": 27.0,
 *   "gripper_feedback_follower_max": 87.5,
 *   "gripper_feedback_offset": 8.0
 * }
 */
struct BimanualGlideConfig {
  /// @brief Network IP address of the left arm's controller
  std::string left_ip_address{"192.168.1.3"};

  /// @brief Left arm model identifier (e.g. "glide")
  std::string left_model{"glide"};

  /// @brief Network IP address of the right arm's controller
  std::string right_ip_address{"192.168.1.2"};

  /// @brief Right arm model identifier (e.g. "glide")
  std::string right_model{"glide"};

  /// @brief Per-tick trajectory time (seconds) passed to set_all_positions /
  /// set_gripper_external_effort. Zero applies the goal immediately.
  double write_moving_time_s{0.0};

  /// @brief Whether this pair participates in the SessionManager's per-episode
  /// lifecycle. Opt-in; defaults to false.
  bool episode_lifecycle_enabled{false};

  /// @brief Optional affine joint remap applied to both arms' read positions:
  /// out[j] = joint_signs[j] * raw[j] + joint_offsets[j]. Empty = identity.
  std::vector<float> joint_signs{};
  std::vector<float> joint_offsets{};

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

  /// @brief Cubic gripper feedback constants (N): leader effort at full grip,
  /// follower effort treated as full grip (the normalizer), and a baseline
  /// opening offset. leader = leader_max·norm^3 + offset, where
  /// norm = clamp(|follower_effort| / follower_max, 0, 1).
  float gripper_feedback_leader_max{27.0f};
  float gripper_feedback_follower_max{87.5f};
  float gripper_feedback_offset{8.0f};

  static BimanualGlideConfig from_json(const nlohmann::json& j) {
    BimanualGlideConfig c;
    if (j.contains("left_ip_address")) j.at("left_ip_address").get_to(c.left_ip_address);
    if (j.contains("left_model")) j.at("left_model").get_to(c.left_model);
    if (j.contains("right_ip_address")) j.at("right_ip_address").get_to(c.right_ip_address);
    if (j.contains("right_model")) j.at("right_model").get_to(c.right_model);
    if (j.contains("write_moving_time_s")) {
      j.at("write_moving_time_s").get_to(c.write_moving_time_s);
    }
    if (j.contains("episode_lifecycle_enabled")) {
      j.at("episode_lifecycle_enabled").get_to(c.episode_lifecycle_enabled);
    }
    if (j.contains("joint_signs")) j.at("joint_signs").get_to(c.joint_signs);
    if (j.contains("joint_offsets")) j.at("joint_offsets").get_to(c.joint_offsets);
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
    if (j.contains("gripper_feedback_leader_max")) {
      j.at("gripper_feedback_leader_max").get_to(c.gripper_feedback_leader_max);
    }
    if (j.contains("gripper_feedback_follower_max")) {
      j.at("gripper_feedback_follower_max").get_to(c.gripper_feedback_follower_max);
    }
    if (j.contains("gripper_feedback_offset")) {
      j.at("gripper_feedback_offset").get_to(c.gripper_feedback_offset);
    }
    return c;
  }

  nlohmann::json to_json() const {
    nlohmann::json j{
      {"left_ip_address", left_ip_address},
      {"left_model", left_model},
      {"right_ip_address", right_ip_address},
      {"right_model", right_model},
      {"write_moving_time_s", write_moving_time_s},
      {"episode_lifecycle_enabled", episode_lifecycle_enabled}
    };
    // Emit the remap only when set, to keep ordinary configs clean.
    if (!joint_signs.empty()) j["joint_signs"] = joint_signs;
    if (!joint_offsets.empty()) j["joint_offsets"] = joint_offsets;
    // Emit per-joint limits only when set.
    if (!position_min.empty()) j["position_min"] = position_min;
    if (!position_max.empty()) j["position_max"] = position_max;
    if (!velocity_max.empty()) j["velocity_max"] = velocity_max;
    if (!effort_max.empty()) j["effort_max"] = effort_max;
    // Emit per-joint tolerances only when set.
    if (!position_tolerance.empty()) j["position_tolerance"] = position_tolerance;
    if (!velocity_tolerance.empty()) j["velocity_tolerance"] = velocity_tolerance;
    if (!effort_tolerance.empty()) j["effort_tolerance"] = effort_tolerance;
    // Gripper feedback constants always emitted: BimanualGlideComponent
    // engages feedback unconditionally (no enable flag), unlike ArmConfig.
    j["gripper_feedback_leader_max"] = gripper_feedback_leader_max;
    j["gripper_feedback_follower_max"] = gripper_feedback_follower_max;
    j["gripper_feedback_offset"] = gripper_feedback_offset;
    return j;
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__BIMANUAL_GLIDE_CONFIG_HPP_
