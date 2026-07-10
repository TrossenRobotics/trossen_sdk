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
 *   "staged_position": [0.0, 1.04, 0.52, 0.63, 0.0, 0.0, 0.0],  // optional
 *   "staging_time_s": 2.0,                                // optional
 *   "episode_lifecycle_enabled": true,                    // optional, default false
 *   "write_moving_time_s": 0.1                            // optional, default 0.0
 * }
 */
struct ArmConfig {
  /// @brief Network IP address of the arm controller
  std::string ip_address{"192.168.1.2"};

  /// @brief Robot model identifier (e.g. "wxai_v0")
  std::string model{"wxai_v0"};

  /// @brief End effector type (e.g. "wxai_v0_follower", "wxai_v0_leader")
  std::string end_effector{"wxai_v0_follower"};

  /// @brief Joint-space home pose the arm moves to when staged. Empty disables
  /// staging. Length must match the arm's joint count (validated downstream by
  /// TrossenArmComponent::configure()).
  std::vector<float> staged_position;

  /// @brief Staging time (seconds): the duration over which an SDK-commanded
  /// point-to-point move runs (staging to home and the return-to-rest move).
  /// Longer = slower, gentler motion. Sized so that a large start-to-goal
  /// difference does not exceed joint velocity limits or produce violent
  /// motion. Optional; 2.0s is a safe default for the supported arms.
  float staging_time_s{2.0f};

  /// @brief Whether this arm participates in the SessionManager's per-episode
  /// lifecycle (staging to its home pose before each episode). Opt-in; defaults to
  /// false so an arm is only re-homed between episodes when explicitly enabled.
  bool episode_lifecycle_enabled{false};

  /// @brief Per-tick trajectory time (seconds) passed to set_all_positions in
  /// TrossenArmComponent::write_joint(). Applies to every write on this arm,
  /// regardless of who issues it (teleop, replay, or policy playback), not just
  /// the policy-client path. Zero applies the goal immediately (libtrossen_arm
  /// treats goal_time < 0.001s as no-interpolation); non-zero smooths the
  /// per-tick motion between successive writes. Opt-in; defaults to 0.0 to
  /// preserve prior immediate-apply behavior byte-for-byte. Validated as
  /// non-negative and finite by TrossenArmComponent::configure().
  /// Tuning trap: keep this below the session's control period. A per-tick
  /// trajectory time longer than the interval to the next write means each goal
  /// is superseded before it is reached, so the arm perpetually chases a moving
  /// target and never settles.
  float write_moving_time_s{0.0f};

  static ArmConfig from_json(const nlohmann::json& j) {
    ArmConfig c;
    if (j.contains("ip_address")) j.at("ip_address").get_to(c.ip_address);
    if (j.contains("model")) j.at("model").get_to(c.model);
    if (j.contains("end_effector")) j.at("end_effector").get_to(c.end_effector);
    if (j.contains("staged_position")) {
      j.at("staged_position").get_to(c.staged_position);
    }
    if (j.contains("staging_time_s")) {
      j.at("staging_time_s").get_to(c.staging_time_s);
    }
    if (j.contains("episode_lifecycle_enabled")) {
      j.at("episode_lifecycle_enabled").get_to(c.episode_lifecycle_enabled);
    }
    if (j.contains("write_moving_time_s")) {
      j.at("write_moving_time_s").get_to(c.write_moving_time_s);
    }
    return c;
  }

  nlohmann::json to_json() const {
    nlohmann::json j{
      {"ip_address", ip_address},
      {"model", model},
      {"end_effector", end_effector},
      {"staging_time_s", staging_time_s},
      {"episode_lifecycle_enabled", episode_lifecycle_enabled},
      {"write_moving_time_s", write_moving_time_s}
    };
    // Emit staging only when configured. TrossenArmComponent::configure()
    // rejects a present-but-wrong-length staged_position, so an empty array
    // would break the no-staging case.
    if (!staged_position.empty()) {
      j["staged_position"] = staged_position;
    }
    return j;
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__ARM_CONFIG_HPP_
