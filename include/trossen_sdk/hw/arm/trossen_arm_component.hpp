/**
 * @file trossen_arm_component.hpp
 * @brief Hardware component wrapper for Trossen Robotics arms.
 */

#ifndef TROSSEN_SDK__HW__ARM__TROSSEN_ARM_COMPONENT_HPP_
#define TROSSEN_SDK__HW__ARM__TROSSEN_ARM_COMPONENT_HPP_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"

#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"
#include "trossen_sdk/utils/one_euro_filter.hpp"

namespace trossen::hw::arm {

/**
 * @brief Hardware component for Trossen Robotics robot arms.
 *
 * Wraps trossen_arm::TrossenArmDriver and provides JSON configuration.
 * Implements teleop::TeleopCapable and supports both joint and cartesian
 * teleop spaces; each space is exposed through a nested adapter sub-object
 * (JointView, CartView) that forwards to space-specific helpers on this
 * class. The controller selects the active space via `as_space_io()`.
 */
class TrossenArmComponent : public HardwareComponent,
                            public teleop::TeleopCapable {
public:
  /**
   * @brief Constructor
   *
   * @param identifier Component identifier
   */
  explicit TrossenArmComponent(std::string identifier) : HardwareComponent(identifier) {}
  ~TrossenArmComponent() override = default;

  // Non-copyable, non-movable: the nested adapter views hold raw back-
  // pointers to `this` that would dangle after a copy or move.
  TrossenArmComponent(const TrossenArmComponent&) = delete;
  TrossenArmComponent& operator=(const TrossenArmComponent&) = delete;
  TrossenArmComponent(TrossenArmComponent&&) = delete;
  TrossenArmComponent& operator=(TrossenArmComponent&&) = delete;

  /**
   * @brief Configure the arm from JSON
   *
   * Expected JSON format:
   * {
   *   "ip_address": "192.168.1.100",
   *   "model": "wxai_v0",
   *   "end_effector": "wxai_v0_follower",
   *   "staged_position": [0, 1.0, 0.5, 0.6, 0, 0, 0],  // optional, joint-space
   *   "staging_time_s": 2.0,       // optional, default 2.0 (stage / rest move)
   *   "write_moving_time_s": 0.1,  // optional, default 0.0 (per-tick smoothing)
   *
   *   // Optional One-Euro adaptive low-pass on outgoing position commands.
   *   // All off / unused unless "smoothing_enabled" is true.
   *   "smoothing_enabled": false,        // optional, default false
   *   "smoothing_gripper": false,        // optional, default false (arm joints only)
   *   "smoothing_min_cutoff_hz": 1.0,    // optional, default 1.0, must be > 0
   *   "smoothing_beta": 0.9,             // optional, default 0.9, must be >= 0
   *   "smoothing_d_cutoff_hz": 1.0       // optional, default 1.0, must be > 0
   * }
   *
   * @param config JSON configuration object
   * @throws std::runtime_error if configuration fails
   */
  void configure(const nlohmann::json& config) override;

  /**
   * @brief Get the type string for this hardware component
   *
   * @return Type identifier
   */
  std::string get_type() const override { return "trossen_arm"; }

  /**
   * @brief Get human-readable component information
   *
   * @return JSON object with component details
   */
  nlohmann::json get_info() const override;

  // ── HardwareComponent: per-episode lifecycle ─────────────────────────────
  // Opt-in via "episode_lifecycle_enabled" in config. When enabled, the
  // SessionManager calls on_pre_episode() to re-home this arm before each
  // episode (it pauses any teleop mirror around the call, so stage() is safe).
  bool is_episode_lifecycle_enabled() const override { return episode_lifecycle_enabled_; }
  void on_pre_episode() override;

  /**
   * @brief Get the underlying hardware driver instance
   *
   * @return Shared pointer to driver
   */
  std::shared_ptr<trossen_arm::TrossenArmDriver> get_hardware() { return driver_; }

  // ── TeleopCapable: space-view accessor ───────────────────────────────────
  // Returns the adapter view for the requested space. Extend the switch to
  // add a new space.
  teleop::TeleopTypeIO* as_space_io(Space space) override {
    switch (space) {
      case Space::Joint:     return &joint_view_;
      case Space::Cartesian: return &cart_view_;
    }
    return nullptr;
  }

  // ── TeleopCapable: shared lifecycle ──────────────────────────────────────
  // Space-agnostic lifecycle hooks. All inputs (role, staging pose,
  // trajectory time) come from members populated at configure() time.
  void prepare_for_teleop() override;
  void end_teleop() override;
  void stage() override;

  /**
   * @brief Apply this arm's affine joint remap in place.
   *
   * Transforms a joint-space vector into the follower's frame using the same
   * signs/offsets as read_joint(): v[j] = joint_signs_[j]*v[j] + joint_offsets_[j].
   * Empty arrays (the follower, or any 1:1 leader) leave the vector unchanged.
   *
   * Exposed publicly so the recording producer can store the *processed*
   * leader stream (the value actually commanded to the follower) rather than
   * raw driver positions — keeping one source of truth for the formula.
   *
   * @param v         Joint-space vector, modified in place.
   * @param derivative When true, the constant offset is dropped and only the
   *                   sign is applied. Correct for velocities and efforts,
   *                   whose frame flips with a joint reversal but which carry
   *                   no positional offset.
   */
  void apply_joint_remap(std::vector<float>& v, bool derivative = false) const;

private:
  // Space-specific IO helpers. Called by the nested adapter views.
  std::vector<float> read_joint();
  void               write_joint(const std::vector<float>& cmd);
  void               summon_joint(const std::vector<float>& cmd);

  /// Follower role: current measured gripper effort (N), or nullopt without a
  /// driver. A sensor read, valid regardless of the gripper's control mode.
  std::optional<float> read_gripper_effort();

  /// Leader role: render gripper force feedback from the follower's measured
  /// gripper effort (N) via the cubic curve, written as an external effort.
  /// Only meaningful when gripper_force_feedback_ is set.
  void apply_gripper_feedback(float follower_gripper_effort);

  std::vector<float> read_cartesian();
  void               write_cartesian(const std::vector<float>& cmd);

  // Adapter views: implement the space child classes and forward to the
  // private helpers above. See the class-level docstring for why this
  // indirection is necessary.
  struct JointView : teleop::JointSpaceTeleop {
    TrossenArmComponent* self;
    explicit JointView(TrossenArmComponent* s) : self(s) {}
    std::vector<float> read() override {
      return self->read_joint();
    }
    void write(const std::vector<float>& cmd) override {
      self->write_joint(cmd);
    }
    void summon(const std::vector<float>& cmd) override {
      self->summon_joint(cmd);
    }
    bool renders_gripper_feedback() const override {
      return self->gripper_force_feedback_;
    }
    std::optional<float> read_gripper_effort() override {
      return self->read_gripper_effort();
    }
    void apply_gripper_feedback(float follower_gripper_effort) override {
      self->apply_gripper_feedback(follower_gripper_effort);
    }
  };

  struct CartView : teleop::CartesianSpaceTeleop {
    TrossenArmComponent* self;
    explicit CartView(TrossenArmComponent* s) : self(s) {}
    std::vector<float> read() override {
      return self->read_cartesian();
    }
    void write(const std::vector<float>& cmd) override {
      self->write_cartesian(cmd);
    }
  };

  JointView joint_view_{this};
  CartView  cart_view_{this};

  std::shared_ptr<trossen_arm::TrossenArmDriver> driver_;
  std::string model_str_;
  std::string end_effector_str_;
  std::string ip_address_;

  /// True if this arm is configured with a leader end-effector. Determines
  /// whether prepare_for_teleop() enters gravity-compensation mode (leader)
  /// or position-mode alignment (follower).
  bool is_leader_{false};

  /// False for a passive leader (no actuators, e.g. the lightweight Trossen
  /// leader). A passive arm only streams joint positions in position mode, so
  /// prepare_for_teleop()/stage()/end_teleop() skip every motion command.
  bool actuated_{true};

  /// Optional affine remap applied in read_joint(): out[j] = joint_signs_[j] *
  /// raw[j] + joint_offsets_[j]. Empty = identity. Used when a leader's joint
  /// frame doesn't map 1:1 onto the follower (e.g. inverted/offset joints).
  std::vector<float> joint_signs_;
  std::vector<float> joint_offsets_;

  /// Leader-only gripper force feedback. When gripper_force_feedback_ is set,
  /// the leader's gripper runs in the mode named by gripper_feedback_mode_ and
  /// the teleop loop renders a reflected force from the follower's measured
  /// gripper effort via the cubic curve. The leader's arm joints can still be
  /// passive.
  bool  gripper_force_feedback_{false};
  float gripper_feedback_leader_max_{27.0f};
  float gripper_feedback_follower_max_{87.5f};
  float gripper_feedback_offset_{8.0f};

  /// True when feedback is rendered with plain effort rather than external
  /// effort. The Rivet's Glide handles were tuned that way; everything else in
  /// the SDK was tuned on external effort, so this defaults to false. Resolved
  /// once in configure() from "gripper_feedback_mode".
  bool  gripper_feedback_plain_effort_{false};

  /// True while the gripper is actually in external-effort mode for feedback
  /// (set by prepare_for_teleop, cleared by end_teleop). Guards end_teleop's
  /// effort release so it never commands external effort on an idle gripper —
  /// e.g. when end_teleop() runs without a preceding prepare_for_teleop()
  /// (the hardware-test park step does exactly that).
  bool  gripper_feedback_engaged_{false};

  /// Optional per-joint operating limits applied to the controller in
  /// configure() (right after driver_->configure). Each, when non-empty, has
  /// one entry per joint; empty leaves the firmware default for that field.
  /// The controller resets these on power cycle, so they are re-applied on
  /// every reconnect.
  std::vector<float> position_min_;
  std::vector<float> position_max_;
  std::vector<float> velocity_max_;
  std::vector<float> effort_max_;

  /// Optional per-joint limit tolerances applied alongside the limits above.
  /// Each, when non-empty, has one entry per joint; empty leaves the firmware
  /// default. Re-applied on every reconnect for the same reason as the limits.
  std::vector<float> position_tolerance_;
  std::vector<float> velocity_tolerance_;
  std::vector<float> effort_tolerance_;

  /// Joint-space pose this arm moves to at session start (via stage()).
  /// Empty = no staging.
  std::vector<float> staged_position_;

  /// Staging time: duration of the point-to-point moves in stage() and the
  /// end_teleop() rest move. Sized to keep motion within joint velocity limits
  /// (no violent moves when start and goal are far apart).
  float staging_time_s_{2.0f};

  /// Whether this arm participates in the per-episode lifecycle (staging before
  /// each episode). Opt-in; parsed from "episode_lifecycle_enabled" in configure().
  bool episode_lifecycle_enabled_{false};

  /// Optional per-joint clamp on outgoing commands, applied in write_joint()
  /// and summon_joint() before smoothing. Empty = no clamping at all; a NaN
  /// entry = that joint is unclamped. See configuration::ArmConfig for why this
  /// is separate from the controller-side position limits.
  std::vector<float> command_position_min_;
  std::vector<float> command_position_max_;

  /// Clamp `pos` in place to the command limits above. No-op when unset.
  void clamp_command(std::vector<double>& pos) const;

  /// Per-write trajectory time passed to set_all_positions in write_joint().
  /// Zero means apply the goal immediately (libtrossen_arm interprets
  /// goal_time < 0.001s as no-interpolation). Non-zero values smooth the
  /// per-tick motion between successive write_joint() calls.
  float write_moving_time_s_{0.0f};

  /// Opt-in one-Euro low-pass on the commands written by write_joint(), and
  /// whether it extends to the gripper channel. Both off by default; see
  /// configuration::ArmConfig for the rationale. Parsed in configure().
  bool smoothing_enabled_{false};
  bool smoothing_gripper_{false};

  /// One-Euro tuning shared by every per-joint filter in cmd_filt_.
  /// See utils::OneEuroFilter for parameter semantics.
  float smoothing_min_cutoff_hz_{1.0f};
  float smoothing_beta_{0.9f};
  float smoothing_d_cutoff_hz_{1.0f};

  /// Per-joint command filters, sized to the arm's joint count in configure()
  /// and only constructed when smoothing is enabled. Reset in
  /// prepare_for_teleop() so filter history never bridges a stopped/restarted
  /// teleop session — stale history would otherwise make the first tick of a
  /// new session compute a derivative against a pose from minutes ago.
  utils::VecOneEuroFilter cmd_filt_;
};

/// @brief Per-joint operating limits and fault tolerances read live from an arm
/// controller. Arrays are parallel, one entry per joint (arm joints in
/// rad / rad·s⁻¹ / N·m, gripper in m / m·s⁻¹ / N).
struct ArmJointLimits {
  std::vector<double> position_min;
  std::vector<double> position_max;
  std::vector<double> velocity_max;
  std::vector<double> effort_max;
  std::vector<double> position_tolerance;
  std::vector<double> velocity_tolerance;
  std::vector<double> effort_tolerance;
};

/// @brief Connect to an arm controller, read its current joint limits, and
/// disconnect.
///
/// Exists so the config UI can seed its per-joint limit and tolerance fields
/// from a real arm instead of numbers nobody measured.
///
/// IMPORTANT — what "current" means. The controller holds whatever limits were
/// last written to it until it is power-cycled, so this returns the firmware
/// defaults only on a freshly booted controller; otherwise it returns whatever
/// was last applied, including values this webapp wrote. That makes a
/// read → save round trip a fixed point (which is what keeps repeated
/// configure() calls from drifting), but it also means a value captured mid-
/// session records the session, not the arm. Capture presets after a power
/// cycle if the intent is "what this arm ships with".
///
/// Spins up a short-lived driver, so the arm must be reachable and NOT held by
/// a running session. Throws std::runtime_error on an unknown model or end
/// effector, or any driver failure.
///
/// @param model         Model identifier; any name the installed driver knows.
/// @param end_effector  End-effector identifier (e.g. "wxai_v0_follower").
/// @param ip_address    Network IP of the arm controller.
ArmJointLimits read_arm_joint_limits(const std::string& model,
                                     const std::string& end_effector,
                                     const std::string& ip_address);

}  // namespace trossen::hw::arm

#endif  // TROSSEN_SDK__HW__ARM__TROSSEN_ARM_COMPONENT_HPP_
