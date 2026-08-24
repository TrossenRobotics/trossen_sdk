/**
 * @file arm_config.hpp
 * @brief Configuration for a robot arm hardware component
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__ARM_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__ARM_CONFIG_HPP_

#include <cmath>
#include <limits>
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
 *   "write_moving_time_s": 0.1,                           // optional, default 0.0
 *
 *   // Command smoothing — optional, all default off / to the values below
 *   "smoothing_enabled": true,
 *   "smoothing_gripper": false,
 *   "smoothing_min_cutoff_hz": 1.0,
 *   "smoothing_beta": 0.9,
 *   "smoothing_d_cutoff_hz": 1.0,
 *
 *   // Controller limit tolerances — optional, one entry per joint; omit an
 *   // array to leave that field at the firmware default
 *   "position_tolerance":[...], "velocity_tolerance": [...],
 *   "effort_tolerance":  [...],
 *
 *   // Host-side clamp on outgoing commands — optional, null = joint unclamped
 *   "command_position_min":
 *     [-1.1, null, null, -1.5358897, -1.5358897, -3.1066861, null],
 *   "command_position_max":
 *     [ 0.8, 3.1066861, 2.3212879, 1.5358897, 1.5358897, 3.1066861, null]
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

  /// @brief Opt-in one-Euro adaptive low-pass on the positions written by
  /// write_joint(). Off by default: it adds lag, and every arm commanded from a
  /// clean source (a policy, a staged move, an SDK-side trajectory) is better
  /// off without it. Turn it on for an arm mirroring a jittery leader — on the
  /// Rivet the Glide handles are hand-held and their raw stream visibly shakes
  /// the followers.
  ///
  /// This filters the COMMAND, and is independent of write_moving_time_s (which
  /// asks the controller to interpolate toward the commanded goal). The two
  /// compose: the filter removes jitter from the target, the moving time softens
  /// the approach to it.
  bool smoothing_enabled{false};

  /// @brief Whether the gripper channel (the last joint) is smoothed too.
  /// Separate from smoothing_enabled and off by default because filtering the
  /// gripper was measured on Rivet hardware to make grasps feel mushy and
  /// late — the operator wants the gripper to track their hand immediately even
  /// while the arm joints are being smoothed.
  bool smoothing_gripper{false};

  /// @brief One-Euro tuning, shared by every per-joint filter instance.
  /// min_cutoff is the cutoff (Hz) at zero speed — lower is smoother but
  /// laggier when nearly still. beta relaxes the filter as the signal moves
  /// faster — higher means less lag during fast motion. d_cutoff is the
  /// derivative's own cutoff and rarely needs tuning.
  ///
  /// The defaults are the values tuned on the Rivet Glide handles and confirmed
  /// on hardware 2026-08-07, so an arm that turns smoothing on without tuning
  /// gets a known-good starting point rather than a neutral one. Both cutoffs
  /// must be > 0; TrossenArmComponent::configure() rejects zero, because a zero
  /// cutoff makes the filter's alpha zero and freezes the output at the first
  /// sample forever rather than failing.
  float smoothing_min_cutoff_hz{1.0f};
  float smoothing_beta{0.9f};
  float smoothing_d_cutoff_hz{1.0f};

  /// @brief Optional per-joint tolerances on the controller's limit checks,
  /// pushed to the controller on connect. Each array, when non-empty, must have
  /// one entry per joint (position in rad / gripper m, velocity in rad·s⁻¹ /
  /// gripper m·s⁻¹, effort in N·m / gripper N). Empty leaves the controller's
  /// firmware default untouched for that field.
  ///
  /// The controller does NOT persist these across a power cycle — they reset to
  /// firmware defaults on reboot — so the SDK re-applies them on every
  /// reconfigure (see TrossenArmComponent).
  ///
  /// TODO(shantanuparab-tr): ship measured defaults. No config in this
  /// repository sets these, so today they are an available option with no
  /// worked example and no tested values. Replace these empty defaults with the
  /// values read off a live Rivet controller once one is powered up.
  std::vector<float> position_tolerance{};
  std::vector<float> velocity_tolerance{};
  std::vector<float> effort_tolerance{};

  /// @brief Optional per-joint clamp applied to outgoing position commands in
  /// write_joint(), before smoothing. Each array, when non-empty, has one entry
  /// per joint; a NaN entry leaves that joint unclamped, so a rig can bound one
  /// axis without inventing bounds for the rest. Serialised as JSON `null`,
  /// which is how "no limit" survives a round trip through a config file.
  ///
  /// Distinct from the arm's own operating limits, which live on the controller
  /// and are enforced there. These bound what teleop is allowed to ASK for, and
  /// exist to keep a follower out of a region its leader can reach but its
  /// workspace cannot — a shelf, a second arm, the base. Clamping host-side means
  /// the command never reaches the controller, so no limit fault is raised and
  /// teleop keeps running with the joint parked at the bound.
  ///
  /// **Two rules generate every bound the Rivet ships, and both matter to anyone
  /// editing them.**
  ///
  /// 1. Every non-J0 bound is the mechanical limit minus exactly 2° — a uniform
  ///    safety margin inside the hardware limit, not arbitrary numbers, so a new
  ///    joint or arm model derives its bound the same way. 2° is 0.0349066 rad,
  ///    NOT 0.035 (which is 2.0054°), and the bounds carry enough digits to hold
  ///    the margin to 2° exactly:
  ///      π/2   − 2° = 1.5358897
  ///      π     − 2° = 3.1066861
  ///      3π/4  − 2° = 2.3212879
  /// 2. **J0 is hand-measured, asymmetric, and mirrored between the two arms:**
  ///    left is −1.1…0.8, right is −0.8…1.1. This is the shoulder-yaw stop that
  ///    keeps the arms out of each other and off the chassis. It is the one value
  ///    that is NOT derivable and must NOT be copied from one side to the other.
  ///    Making the asymmetry uniform points both arms into the same space.
  std::vector<float> command_position_min{};
  std::vector<float> command_position_max{};

  /// @brief Parse an array whose entries may be JSON `null`, mapping null to
  /// NaN. Used for the command clamps, where a per-joint "no bound" has to
  /// survive both directions of a config round trip.
  static std::vector<float> parse_nullable_limits(const nlohmann::json& arr) {
    std::vector<float> out;
    out.reserve(arr.size());
    for (const auto& entry : arr) {
      out.push_back(
        entry.is_null() ? std::numeric_limits<float>::quiet_NaN() : entry.get<float>());
    }
    return out;
  }

  /// @brief Inverse of parse_nullable_limits: NaN becomes JSON null.
  static nlohmann::json dump_nullable_limits(const std::vector<float>& v) {
    auto arr = nlohmann::json::array();
    for (const float x : v) {
      if (std::isnan(x)) {
        arr.push_back(nullptr);
      } else {
        arr.push_back(x);
      }
    }
    return arr;
  }

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
    if (j.contains("smoothing_enabled")) {
      j.at("smoothing_enabled").get_to(c.smoothing_enabled);
    }
    if (j.contains("smoothing_gripper")) {
      j.at("smoothing_gripper").get_to(c.smoothing_gripper);
    }
    if (j.contains("smoothing_min_cutoff_hz")) {
      j.at("smoothing_min_cutoff_hz").get_to(c.smoothing_min_cutoff_hz);
    }
    if (j.contains("smoothing_beta")) {
      j.at("smoothing_beta").get_to(c.smoothing_beta);
    }
    if (j.contains("smoothing_d_cutoff_hz")) {
      j.at("smoothing_d_cutoff_hz").get_to(c.smoothing_d_cutoff_hz);
    }
    if (j.contains("position_tolerance")) {
      j.at("position_tolerance").get_to(c.position_tolerance);
    }
    if (j.contains("velocity_tolerance")) {
      j.at("velocity_tolerance").get_to(c.velocity_tolerance);
    }
    if (j.contains("effort_tolerance")) {
      j.at("effort_tolerance").get_to(c.effort_tolerance);
    }
    if (j.contains("command_position_min")) {
      c.command_position_min = parse_nullable_limits(j.at("command_position_min"));
    }
    if (j.contains("command_position_max")) {
      c.command_position_max = parse_nullable_limits(j.at("command_position_max"));
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
    // Emit the smoothing tuning only when smoothing is on: the constants are
    // meaningless while it is off, and emitting them would put four keys into
    // every ordinary arm's config.
    if (smoothing_enabled) {
      j["smoothing_enabled"] = smoothing_enabled;
      j["smoothing_gripper"] = smoothing_gripper;
      j["smoothing_min_cutoff_hz"] = smoothing_min_cutoff_hz;
      j["smoothing_beta"] = smoothing_beta;
      j["smoothing_d_cutoff_hz"] = smoothing_d_cutoff_hz;
    }
    // Emit each tolerance array only when set. An empty array is not equivalent:
    // TrossenArmComponent::configure() reads "absent" as "leave the controller's
    // firmware default alone", and a present-but-empty array would still take
    // the read-modify-write path against the live limits.
    if (!position_tolerance.empty()) j["position_tolerance"] = position_tolerance;
    if (!velocity_tolerance.empty()) j["velocity_tolerance"] = velocity_tolerance;
    if (!effort_tolerance.empty()) j["effort_tolerance"] = effort_tolerance;
    // Same emit-only-when-set rule; NaN entries go back out as null.
    if (!command_position_min.empty()) {
      j["command_position_min"] = dump_nullable_limits(command_position_min);
    }
    if (!command_position_max.empty()) {
      j["command_position_max"] = dump_nullable_limits(command_position_max);
    }
    return j;
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__ARM_CONFIG_HPP_
