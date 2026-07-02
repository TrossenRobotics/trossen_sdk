/**
 * @file trossen_arm_component.hpp
 * @brief Hardware component wrapper for Trossen Robotics arms.
 */

#ifndef TROSSEN_SDK__HW__ARM__TROSSEN_ARM_COMPONENT_HPP_
#define TROSSEN_SDK__HW__ARM__TROSSEN_ARM_COMPONENT_HPP_

#include <memory>
#include <string>
#include <optional>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"

#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"

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
   *   "teleop_moving_time_s": 2.0         // optional, default 2.0
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
  /// the leader's gripper runs in external-effort mode and the teleop loop
  /// renders a reflected force from the follower's measured gripper effort via
  /// the cubic curve. The leader's arm joints can still be passive.
  bool  gripper_force_feedback_{false};
  float gripper_feedback_leader_max_{27.0f};
  float gripper_feedback_follower_max_{87.5f};
  float gripper_feedback_offset_{8.0f};

  /// True while the gripper is actually in external-effort mode for feedback
  /// (set by prepare_for_teleop, cleared by end_teleop). Guards end_teleop's
  /// effort release so it never commands external effort on an idle gripper —
  /// e.g. when end_teleop() runs without a preceding prepare_for_teleop()
  /// (the hardware-test park step does exactly that).
  bool  gripper_feedback_engaged_{false};

  /// Joint-space pose this arm moves to at session start (via stage()).
  /// Empty = no staging.
  std::vector<float> staged_position_;

  /// Trajectory time used by stage() and the end_teleop() rest move.
  float teleop_moving_time_s_{2.0f};
};

}  // namespace trossen::hw::arm

#endif  // TROSSEN_SDK__HW__ARM__TROSSEN_ARM_COMPONENT_HPP_
