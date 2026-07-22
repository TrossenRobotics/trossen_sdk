/**
 * @file trossen_arm_component.hpp
 * @brief Hardware component wrapper for Trossen Robotics arms.
 */

#ifndef TROSSEN_SDK__HW__COMPOSITE__BIMANUAL_GLIDE_COMPONENT_HPP_
#define TROSSEN_SDK__HW__COMPOSITE__BIMANUAL_GLIDE_COMPONENT_HPP_

#include <memory>
#include <string>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"

#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"

namespace trossen::hw::bimanual_glide {

/**
 * @brief Hardware component for Trossen Robotics robot arms.
 *
 * Wraps 2 trossen_arm::TrossenArmDriver and provides JSON configuration.
 * Implements teleop::TeleopCapable and supports both joint and cartesian
 * teleop spaces; each space is exposed through a nested adapter sub-object
 * that forwards to space-specific helpers on this
 * class. The controller selects the active space via `as_space_io()`.
 */
class BimanualGlideComponent : public HardwareComponent,
                            public teleop::TeleopCapable {
public:
  /**
   * @brief Constructor
   *
   * @param identifier Component identifier
   */
  explicit BimanualGlideComponent(std::string identifier) : HardwareComponent(identifier) {}
  ~BimanualGlideComponent() override = default;

  // Non-copyable, non-movable: the nested adapter views hold raw back-
  // pointers to `this` that would dangle after a copy or move.
  BimanualGlideComponent(const BimanualGlideComponent&) = delete;
  BimanualGlideComponent& operator=(const BimanualGlideComponent&) = delete;
  BimanualGlideComponent(BimanualGlideComponent&&) = delete;
  BimanualGlideComponent& operator=(BimanualGlideComponent&&) = delete;

  /**
   * @brief Configure the arm from JSON
   *
   * Expected JSON format:
   * {
   *   "left_ip_address": "192.168.1.100",
   *   "left_model": "glide_right",
   *   "right_ip_address": "192.168.1.101",
   *   "right_model": "glide_left",
   *   "write_moving_time_s": 0.2,
   *   "episode_lifecycle_enabled": true,
   *   "joint_signs": [1, 1, ...],
   *   "left_joint_offsets": [0, 0, ...],
   *   "right_joint_offsets": [0, 0, ...],
   *   "position_min": [...],
   *   "position_max": [...],
   *   "velocity_max": [...],
   *   "effort_max": [...],
   *   "position_tolerance": [...],
   *   "velocity_tolerance": [...],
   *   "effort_tolerance": [...],
   *   "gripper_feedback_leader_max": 27.0,
   *   "gripper_feedback_follower_max": 87.5,
   *   "gripper_feedback_offset": 8.0
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
  std::string get_type() const override { return "bimanual_glide"; }

  /**
   * @brief Get human-readable component information
   *
   * @return JSON object with component details
   */
  nlohmann::json get_info() const override;

  // ── HardwareComponent: per-episode lifecycle ─────────────────────────────
  // TODO: @schromya check what this does (if anything)
  // Opt-in via "episode_lifecycle_enabled" in config. When enabled, the
  // SessionManager calls on_pre_episode() to re-home this arm before each
  // episode (it pauses any teleop mirror around the call, so stage() is safe).
  bool is_episode_lifecycle_enabled() const override { return episode_lifecycle_enabled_; }
  void on_pre_episode() override;

  /**
   * @brief Get the underlying hardware driver instances
   *
   * @return Vector of {left, right} driver shared pointers
   */
  std::vector<std::shared_ptr<trossen_arm::TrossenArmDriver>> get_hardware() {
    return {left_driver_, right_driver_};
  }

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
   * Transforms a joint-space vector into the follower's frame:
   * v[j] = joint_signs_[j]*v[j] + offsets[j]. Empty arrays (the follower, or
   * any 1:1 leader) leave the vector unchanged.
   *
   * Exposed publicly so the recording producer can store the *processed*
   * leader stream (the value actually commanded to the follower) rather than
   * raw driver positions — keeping one source of truth for the formula.
   *
   * @param v         Joint-space vector, modified in place.
   * @param offsets   Per-joint offsets for this arm (joint_offsets_left_ or
   *                  joint_offsets_right_). Empty = no offset.
   * @param derivative When true, the constant offset is dropped and only the
   *                   sign is applied. Correct for velocities and efforts,
   *                   whose frame flips with a joint reversal but which carry
   *                   no positional offset.
   */
  void apply_joint_remap(
    std::vector<float>& v, const std::vector<float>& offsets, bool derivative = false) const;


private:
  // Space-specific IO helpers. Called by the nested adapter views.
  std::vector<float> read_joint();
  std::vector<float> read_cartesian();


  /**
   * @brief Read joint efforts from both arm triggers
   * @return Vector (2) of joint efforts (N) for gripper [left_q6, right_q6]
   */
  std::vector<float> read_gripper_effort();

  /**
   * @brief Write joint efforts to both arm triggers
   * @param follower_gripper_effort Vector (2) of joint efforts (N) for gripper [left_q6, right_q6]
   */
  void apply_gripper_feedback(const std::vector<float>& follower_gripper_effort);


  // Adapter views: implement the space child classes and forward to the
  // private helpers above. See the class-level docstring for why this
  // indirection is necessary.
  struct JointView : teleop::JointSpaceTeleop {
    BimanualGlideComponent* self;
    explicit JointView(BimanualGlideComponent* s) : self(s) {}

    /**
     * @brief Read the joint positions from both arms
     * @return Vector () of joint positions [left_q0, left_q1, ..., right_q0, right_q1, ...]
     */
    std::vector<float> read() override {
      return self->read_joint();
    }

    /** @brief Does nothing */
    void write(const std::vector<float>& cmd) override {}

    /** @brief Does nothing */
    void summon(const std::vector<float>& cmd) override {}

  /**
   * @brief  Determine if gripper feedback is rendered
   * @return True if uses gripper feedback
   */
    bool renders_gripper_feedback() const override {return true;}

    std::vector<float> read_multiple_gripper_efforts() override {
      return self->read_gripper_effort();
    }

  /**
   * @brief Write joint efforts to both arm grippers
   * @param follower_gripper_effort Vector (2) of joint efforts (N) for gripper [left_q6, right_q6]
   */
    void apply_multiple_gripper_feedback(std::vector<float> follower_gripper_effort) override {
      self->apply_gripper_feedback(follower_gripper_effort);
    }
  };

  struct CartView : teleop::CartesianSpaceTeleop {
    BimanualGlideComponent* self;
    explicit CartView(BimanualGlideComponent* s) : self(s) {}
    /**
     * @brief Read the cartesian positions from both arms
     * @return Vector (14) of joint positions [left_x, left_y, left_z, left_rx, left_ry, left_rz,
     *         left_gripper_m, right_x, right_y, ...]
     */
    std::vector<float> read() override {
      return self->read_cartesian();
    }
    /** @brief Does nothing */
    void write(const std::vector<float>& cmd) override {
      // Do nothing
    }
  };

  JointView joint_view_{this};
  CartView  cart_view_{this};


  std::shared_ptr<trossen_arm::TrossenArmDriver> left_driver_;
  std::shared_ptr<trossen_arm::TrossenArmDriver> right_driver_;
  std::string left_model_str_;
  std::string left_ip_address_;
  std::string right_model_str_;
  std::string right_ip_address_;
  size_t njoints_;  // Joints per arm (same for both)

  /// Whether this arm participates in the per-episode lifecycle (staging before
  /// each episode). Opt-in; parsed from "episode_lifecycle_enabled" in configure().
  bool episode_lifecycle_enabled_{false};

  /// Per-write trajectory time passed to set_all_positions in write_joint().
  /// Zero means apply the goal immediately (libtrossen_arm interprets
  /// goal_time < 0.001s as no-interpolation). Non-zero values smooth the
  /// per-tick motion between successive write_joint() calls.
  float write_moving_time_s_{0.2f};

  /// Optional affine remap applied in read_joint(): out[j] = joint_signs_[j] *
  /// raw[j] + joint_offsets_{left,right}_[j]. Empty = identity. Used when a
  /// leader's joint frame doesn't map 1:1 onto the follower (e.g.
  /// inverted/offset joints). Signs are shared across arms; offsets are per-arm
  std::vector<float> joint_signs_;
  std::vector<float> joint_offsets_left_;
  std::vector<float> joint_offsets_right_;

  /// Leader-only gripper force feedback. When gripper_force_feedback_ is set,
  /// the leader's gripper runs in external-effort mode and the teleop loop
  /// renders a reflected force from the follower's measured gripper effort via
  /// the cubic curve. The leader's arm joints can still be passive.
  float gripper_feedback_leader_max_{27.0f};
  float gripper_feedback_follower_max_{87.5f};
  float gripper_feedback_offset_{8.0f};

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

};

}  // namespace trossen::hw::bimanual_glide

#endif  // TROSSEN_SDK__HW__COMPOSITE__BIMANUAL_GLIDE_COMPONENT_HPP_
