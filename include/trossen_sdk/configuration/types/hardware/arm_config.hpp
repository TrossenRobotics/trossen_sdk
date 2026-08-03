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
  /// write_joint(). Zero applies the goal immediately (libtrossen_arm treats
  /// goal_time < 0.001s as no-interpolation); non-zero smooths the per-tick
  /// motion between successive writes. Used by the policy-client playback path
  /// on top of its own EMA output filter.
  float write_moving_time_s{0.0f};

  /// @brief Whether the arm has actuators. A passive leader (e.g. the
  /// lightweight Trossen leader) only streams joint positions and cannot be
  /// commanded, so teleop staging / mode setup / rest moves are skipped.
  bool actuated{true};

  /// @brief Opt-in one-Euro adaptive low-pass on the positions written by
  /// write_joint(). Off by default: it adds lag, and every arm that is
  /// commanded from a clean source (a policy, a staged move, an SDK-side
  /// trajectory) is better off without it. Turn it on for an arm mirroring a
  /// jittery leader — on the Rivet the Glide handles are hand-held and their
  /// raw stream visibly shakes the followers.
  ///
  /// This is a filter on the COMMAND, and is independent of
  /// write_moving_time_s (which asks the controller to interpolate toward the
  /// commanded goal). The two compose: the filter removes jitter from the
  /// target, the moving time softens the approach to it.
  bool smoothing_enabled{false};

  /// @brief Whether the gripper channel (the last joint) is smoothed too.
  /// Separate from smoothing_enabled and off by default because filtering the
  /// gripper was measured on Rivet hardware to make grasps feel mushy and
  /// late — the operator wants the gripper to track their hand immediately
  /// even when the arm joints are being smoothed.
  bool smoothing_gripper{false};

  /// @brief One-Euro tuning, shared by every per-joint filter instance.
  /// min_cutoff is the cutoff (Hz) at zero speed — lower is smoother but
  /// laggier when nearly still. beta relaxes the filter as the signal moves
  /// faster — higher means less lag during fast motion. d_cutoff is the
  /// derivative's own cutoff and rarely needs tuning. The defaults are the
  /// values tuned on the Rivet Glide handles.
  float smoothing_min_cutoff_hz{1.0f};
  float smoothing_beta{0.9f};
  float smoothing_d_cutoff_hz{1.0f};

  /// @brief Optional affine joint remap applied to this arm's read positions:
  /// out[j] = joint_signs[j] * raw[j] + joint_offsets[j]. Empty = identity.
  /// Used for a leader whose joint frame doesn't map 1:1 onto the follower.
  std::vector<float> joint_signs{};
  std::vector<float> joint_offsets{};

  /// @brief Leader-only: render gripper force feedback. When true, the teleop
  /// loop reflects the FOLLOWER's measured gripper effort back onto this
  /// (actuated) gripper via a cubic curve, so the operator feels the grasp.
  /// The leader's arm joints may still be passive — only the gripper needs a
  /// motor. The follower gripper stays plain position passthrough.
  bool gripper_force_feedback{false};

  /// @brief Cubic feedback constants (N), from the bilateral reference:
  /// leader effort at full grip, follower effort treated as full grip (the
  /// normalizer), and a baseline opening offset that keeps the leader gripper
  /// open when nothing is grasped. leader = leader_max·norm^3 + offset, where
  /// norm = clamp(|follower_effort| / follower_max, 0, 1).
  float gripper_feedback_leader_max{27.0f};
  float gripper_feedback_follower_max{87.5f};
  float gripper_feedback_offset{8.0f};

  /// @brief Which driver mode renders gripper feedback: "external_effort"
  /// (default) or "effort".
  ///
  /// The Rivet's Glide handles were tuned on plain "effort" — on that hardware
  /// external-effort mode fights the operator instead of yielding to them. Every
  /// other robot in the SDK was tuned on external-effort, so this stays a
  /// per-arm choice rather than a global switch; the default preserves the
  /// existing behaviour everywhere it is not set.
  std::string gripper_feedback_mode{"external_effort"};

  /// @brief Optional per-joint operating limits pushed to the controller on
  /// connect. Each array, when non-empty, must have one entry per joint (arm
  /// joints in rad / rad·s⁻¹ / N·m, gripper in m / m·s⁻¹ / N). Empty = leave
  /// the controller's firmware default untouched for that field.
  ///
  /// The controller clips commands to these limits and does NOT persist them
  /// across a power cycle — they reset to firmware defaults on reboot — so the
  /// SDK re-applies them on every reconfigure (see TrossenArmComponent).
  std::vector<float> position_min{};
  std::vector<float> position_max{};
  std::vector<float> velocity_max{};
  std::vector<float> effort_max{};

  /// @brief Optional per-joint limit tolerances pushed to the controller
  /// alongside the limits above. Each array, when non-empty, must have one
  /// entry per joint (position in rad / gripper m, velocity in rad·s⁻¹ /
  /// gripper m·s⁻¹, effort in N·m / gripper N). Empty = leave the controller's
  /// firmware default untouched for that field. Same non-persistent-across-
  /// power-cycle behaviour as the limits, so re-applied on every reconfigure.
  std::vector<float> position_tolerance{};
  std::vector<float> velocity_tolerance{};
  std::vector<float> effort_tolerance{};

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
    if (j.contains("actuated")) j.at("actuated").get_to(c.actuated);
    if (j.contains("smoothing_enabled"))
      j.at("smoothing_enabled").get_to(c.smoothing_enabled);
    if (j.contains("smoothing_gripper"))
      j.at("smoothing_gripper").get_to(c.smoothing_gripper);
    if (j.contains("smoothing_min_cutoff_hz"))
      j.at("smoothing_min_cutoff_hz").get_to(c.smoothing_min_cutoff_hz);
    if (j.contains("smoothing_beta"))
      j.at("smoothing_beta").get_to(c.smoothing_beta);
    if (j.contains("smoothing_d_cutoff_hz"))
      j.at("smoothing_d_cutoff_hz").get_to(c.smoothing_d_cutoff_hz);
    if (j.contains("joint_signs")) j.at("joint_signs").get_to(c.joint_signs);
    if (j.contains("joint_offsets")) j.at("joint_offsets").get_to(c.joint_offsets);
    if (j.contains("gripper_force_feedback"))
      j.at("gripper_force_feedback").get_to(c.gripper_force_feedback);
    if (j.contains("gripper_feedback_leader_max"))
      j.at("gripper_feedback_leader_max").get_to(c.gripper_feedback_leader_max);
    if (j.contains("gripper_feedback_follower_max"))
      j.at("gripper_feedback_follower_max").get_to(c.gripper_feedback_follower_max);
    if (j.contains("gripper_feedback_offset"))
      j.at("gripper_feedback_offset").get_to(c.gripper_feedback_offset);
    if (j.contains("gripper_feedback_mode"))
      j.at("gripper_feedback_mode").get_to(c.gripper_feedback_mode);
    if (j.contains("position_min")) j.at("position_min").get_to(c.position_min);
    if (j.contains("position_max")) j.at("position_max").get_to(c.position_max);
    if (j.contains("velocity_max")) j.at("velocity_max").get_to(c.velocity_max);
    if (j.contains("effort_max")) j.at("effort_max").get_to(c.effort_max);
    if (j.contains("position_tolerance"))
      j.at("position_tolerance").get_to(c.position_tolerance);
    if (j.contains("velocity_tolerance"))
      j.at("velocity_tolerance").get_to(c.velocity_tolerance);
    if (j.contains("effort_tolerance"))
      j.at("effort_tolerance").get_to(c.effort_tolerance);
    return c;
  }

  nlohmann::json to_json() const {
    nlohmann::json j{
      {"ip_address", ip_address},
      {"model", model},
      {"end_effector", end_effector},
      {"staging_time_s", staging_time_s},
      {"episode_lifecycle_enabled", episode_lifecycle_enabled},
      {"write_moving_time_s", write_moving_time_s},
      {"actuated", actuated}
    };
    // Emit staging only when configured. TrossenArmComponent::configure()
    // rejects a present-but-wrong-length staged_position, so an empty array
    // would break the no-staging case.
    if (!staged_position.empty()) {
      j["staged_position"] = staged_position;
    }
    // Emit the remap only when set, to keep ordinary arm configs clean.
    if (!joint_signs.empty()) j["joint_signs"] = joint_signs;
    if (!joint_offsets.empty()) j["joint_offsets"] = joint_offsets;
    // Emit gripper feedback tuning only when enabled, same reasoning.
    if (gripper_force_feedback) {
      j["gripper_force_feedback"] = gripper_force_feedback;
      j["gripper_feedback_leader_max"] = gripper_feedback_leader_max;
      j["gripper_feedback_follower_max"] = gripper_feedback_follower_max;
      j["gripper_feedback_offset"] = gripper_feedback_offset;
      j["gripper_feedback_mode"] = gripper_feedback_mode;
    }
    // Emit smoothing tuning only when enabled, same reasoning as the gripper
    // feedback block above — the constants are meaningless while it is off.
    if (smoothing_enabled) {
      j["smoothing_enabled"] = smoothing_enabled;
      j["smoothing_gripper"] = smoothing_gripper;
      j["smoothing_min_cutoff_hz"] = smoothing_min_cutoff_hz;
      j["smoothing_beta"] = smoothing_beta;
      j["smoothing_d_cutoff_hz"] = smoothing_d_cutoff_hz;
    }
    // Emit per-joint limits only when set, to keep ordinary arm configs clean.
    if (!position_min.empty()) j["position_min"] = position_min;
    if (!position_max.empty()) j["position_max"] = position_max;
    if (!velocity_max.empty()) j["velocity_max"] = velocity_max;
    if (!effort_max.empty()) j["effort_max"] = effort_max;
    // Emit per-joint tolerances only when set, same reasoning.
    if (!position_tolerance.empty()) j["position_tolerance"] = position_tolerance;
    if (!velocity_tolerance.empty()) j["velocity_tolerance"] = velocity_tolerance;
    if (!effort_tolerance.empty()) j["effort_tolerance"] = effort_tolerance;
    return j;
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__ARM_CONFIG_HPP_
