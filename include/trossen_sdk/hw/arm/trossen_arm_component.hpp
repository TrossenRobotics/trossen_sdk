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
   *   "actuated": false,           // optional, default true (false = read-only arm)
   *
   *   // Host-side clamp on outgoing position commands. Optional; one entry per
   *   // joint, null leaves that joint unclamped. Applied before smoothing.
   *   "command_position_min": [-1.1, null, null, ...],
   *   "command_position_max": [ 0.8, 3.1066861, ...],
   *
   *   // Controller limit tolerances. Optional; one entry per joint, omit an
   *   // array to leave that field at the controller's firmware default.
   *   "position_tolerance": [...],
   *   "velocity_tolerance": [...],
   *   "effort_tolerance":   [...],
   *
   *   // One-Euro low-pass on outgoing position commands.
   *   // All ignored unless "smoothing_enabled" is true.
   *   "smoothing_enabled": false,      // optional, default false
   *   "smoothing_gripper": false,      // optional, default false (arm joints only)
   *   "smoothing_min_cutoff_hz": 1.0,  // optional, default 1.0, must be > 0
   *   "smoothing_beta": 0.9,           // optional, default 0.9, must be >= 0
   *   "smoothing_d_cutoff_hz": 1.0     // optional, default 1.0, must be > 0
   * }
   *
   * See configuration::ArmConfig for what the clamp bounds mean and how the
   * Rivet's values are derived.
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
      case Space::Base:      return nullptr;
      case Space::Count:     return nullptr;
    }
    return nullptr;
  }

  // ── TeleopCapable: shared lifecycle ──────────────────────────────────────
  // Space-agnostic lifecycle hooks. All inputs (role, staging pose,
  // trajectory time) come from members populated at configure() time.
  void prepare_for_teleop() override;
  void end_teleop() override;
  void stage() override;

private:
  // Space-specific IO helpers. Called by the nested adapter views.
  std::vector<float> read_joint();
  void               write_joint(const std::vector<float>& cmd);

  /// Follower role: current measured gripper effort (N), or nullopt without a
  /// driver. A sensor read, valid regardless of the gripper's control mode.
  std::optional<float> read_gripper_effort();

  /// Leader role: render gripper force feedback from the follower's measured
  /// gripper effort (N) via the cubic curve. Only meaningful when
  /// gripper_force_feedback_ is set.
  void apply_gripper_feedback(float follower_gripper_effort);

  std::vector<float> read_cartesian();
  void               write_cartesian(const std::vector<float>& cmd);

  /// Clamp `pos` in place to command_position_min_ / command_position_max_.
  /// No-op when both are empty, and per joint when that entry is NaN.
  void clamp_command(std::vector<double>& pos) const;

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
    // Gripper force-feedback channel. Only the joint view carries it: the
    // reflected force is a gripper effort, which has no cartesian analogue.
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

  /// Whether this arm has actuators. A passive leader is read-only: it streams
  /// joint positions and cannot be commanded, so stage(), the teleop mode
  /// setup, and the end_teleop() rest move are all skipped. Parsed from
  /// "actuated" in configure(); defaults true, so every existing arm keeps its
  /// current behaviour.
  bool actuated_{true};

  /// Leader-only: whether to reflect the follower's grasp onto this gripper,
  /// and the cubic curve that shapes it. See configuration::ArmConfig.
  bool gripper_force_feedback_{false};
  float gripper_feedback_leader_max_{27.0f};
  float gripper_feedback_follower_max_{87.5f};
  float gripper_feedback_offset_{8.0f};

  /// Whether prepare_for_teleop() put this arm's gripper into effort mode, so
  /// end_teleop() releases it only when it was actually engaged. Not merely
  /// tidiness: end_teleop() can be called with no preceding
  /// prepare_for_teleop() — the hardware-test park step does exactly that — and
  /// commanding effort on a gripper still in idle mode is a controller error.
  bool gripper_effort_engaged_{false};

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

  /// Per-write trajectory time passed to set_all_positions in write_joint().
  /// Zero means apply the goal immediately (libtrossen_arm interprets
  /// goal_time < 0.001s as no-interpolation). Non-zero values smooth the
  /// per-tick motion between successive write_joint() calls.
  float write_moving_time_s_{0.0f};

  /// Optional per-joint tolerances on the controller's limit checks, pushed in
  /// configure() right after the driver connects. Each, when non-empty, has one
  /// entry per joint; empty leaves the firmware default for that field. The
  /// controller resets these on power cycle, so they are re-applied on every
  /// reconnect rather than assumed to have survived.
  std::vector<float> position_tolerance_;
  std::vector<float> velocity_tolerance_;
  std::vector<float> effort_tolerance_;

  /// Optional per-joint clamp on outgoing commands, applied in write_joint()
  /// before smoothing. Empty = no clamping at all; a NaN entry = that joint is
  /// unclamped. See configuration::ArmConfig for why this is separate from the
  /// controller-side position limits, and how the Rivet's bounds are derived.
  std::vector<float> command_position_min_;
  std::vector<float> command_position_max_;

  /// Opt-in one-Euro low-pass on the commands written by write_joint(), and
  /// whether it extends to the gripper channel. Both off by default; see
  /// configuration::ArmConfig for the rationale. Parsed in configure().
  bool smoothing_enabled_{false};
  bool smoothing_gripper_{false};

  /// One-Euro tuning shared by every per-joint filter in cmd_filt_.
  /// See utils::OneEuroFilter for parameter semantics. configure() rejects a
  /// non-positive cutoff, which would otherwise freeze the output silently.
  float smoothing_min_cutoff_hz_{1.0f};
  float smoothing_beta_{0.9f};
  float smoothing_d_cutoff_hz_{1.0f};

  /// Per-joint command filters, sized to the arm's joint count in configure()
  /// and only constructed when smoothing is enabled. Reset in
  /// prepare_for_teleop() so filter history never bridges a stopped and
  /// restarted teleop session — stale history would otherwise have the first
  /// tick of a new session compute a derivative against a pose from minutes ago,
  /// spiking the adaptive cutoff exactly when the arm is nearest the operator.
  utils::VecOneEuroFilter cmd_filt_;
};

}  // namespace trossen::hw::arm

#endif  // TROSSEN_SDK__HW__ARM__TROSSEN_ARM_COMPONENT_HPP_
