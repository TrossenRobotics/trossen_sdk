/**
 * @file trossen_arm_component.hpp
 * @brief Hardware component wrapper for Trossen Robotics arms.
 */

#ifndef TROSSEN_SDK__HW__ARM__RIVET_COMPONENT_HPP_
#define TROSSEN_SDK__HW__ARM__RIVET_COMPONENT_HPP_

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"
#include "trossen_base/trossen_base.hpp"

#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"

namespace trossen::hw::rivet {

/**
 * @brief Hardware component for Trossen Robotics robot arms.
 *
 * Wraps 2 trossen_arm::TrossenArmDriver and provides JSON configuration.
 * Implements teleop::TeleopCapable and supports both joint and cartesian
 * teleop spaces; each space is exposed through a nested adapter sub-object
 * that forwards to space-specific helpers on this
 * class. The controller selects the active space via `as_space_io()`.
 */
class RivetComponent : public HardwareComponent,
                            public teleop::TeleopCapable {
public:
  /**
   * @brief Constructor
   *
   * @param identifier Component identifier
   */
  explicit RivetComponent(std::string identifier) : HardwareComponent(identifier) {}
  ~RivetComponent() override;

  // Non-copyable, non-movable: the nested adapter views hold raw back-
  // pointers to `this` that would dangle after a copy or move.
  RivetComponent(const RivetComponent&) = delete;
  RivetComponent& operator=(const RivetComponent&) = delete;
  RivetComponent(RivetComponent&&) = delete;
  RivetComponent& operator=(RivetComponent&&) = delete;

  /**
   * @brief Configure the arm from JSON
   *
   * Expected JSON format:
   * {
   *   "left_ip_address": "192.168.1.5",
   *   "left_model": "pro",
   *   "right_ip_address": "192.168.1.4",
   *   "right_model": "pro",
   *   "write_moving_time_s": 0.2,
   *   "staging_time_s": 2.0
   *   "end_effector": "wxai_v0_follower",
   *   "episode_lifecycle_enabled": true,
   *   "position_min": [...],
   *   "position_max": [...],
   *   "velocity_max": [...],
   *   "effort_max": [...],
   *   "position_tolerance": [...],
   *   "velocity_tolerance": [...],
   *   "effort_tolerance": [...],
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
  std::string get_type() const override { return "rivet"; }

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
   * @return Vector of {left, right, base} driver shared pointers
   */
  std::vector<std::shared_ptr<trossen_arm::TrossenArmDriver>> get_arm_hardware() {
    return {left_driver_, right_driver_};
  }
  std::vector<std::shared_ptr<trossen_base::TrossenBase>> get_base_hardware() {
    return {base_driver_};
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

private:
  // Space-specific IO helpers. Called by the nested adapter views.
  std::vector<float> read_joint();
  void               write_joint(const std::vector<float>& cmd);
  void               summon_joint(const std::vector<float>& cmd);

  std::vector<float> read_cartesian();
  void               write_cartesian(const std::vector<float>& cmd);

  /**
   * @brief Read joint efforts from both arm triggers
   * @return Vector (2) of joint efforts (N) for gripper [left_q6, right_q6]
   */
  std::vector<float> read_gripper_effort();

  /**
   * @brief Background loop that ticks the base driver at BASE_UPDATE_HZ
   */
  void base_update_loop();

  // Adapter views: implement the space child classes and forward to the
  // private helpers above. See the class-level docstring for why this
  // indirection is necessary.
  struct JointView : teleop::JointSpaceTeleop {
    RivetComponent* self;
    explicit JointView(RivetComponent* s) : self(s) {}

    /**
     * @brief Read the arm joint positions, base velocities, and lift velocities
     * @return Vector ()  [left_q0, left_q1, left_q2, left_q3, left_q4, left_q5, left_q6,
     *         right_q0, right_q1, right_q2, right_q3, right_q4, right_q5, right_q6,
     *         base_vx, base_vy, base_vr, base_vlift]
     */
    std::vector<float> read() override {
      return self->read_joint();
    }
    /**
     * @brief Write the arm joint positions, base velocities, and lift velocities
     * @param cmd Vector (18)  [left_q0, left_q1, left_q2, left_q3, left_q4, left_q5, left_q6,
     *         right_q0, right_q1, right_q2, right_q3, right_q4, right_q5, right_q6,
     *         base_vx, base_vy, base_vr, base_vlift]
     */
    void write(const std::vector<float>& cmd) override {
      self->write_joint(cmd);
    }
    /** @brief Moves robots to summoning position */
    void summon(const std::vector<float>& cmd) override {
      self->summon_joint(cmd);
    }
    /**
     * @brief  Determine if gripper feedback is rendered
     * @return True if uses gripper feedback
     */
    bool renders_gripper_feedback() const override {return false;}

    std::vector<float> read_multiple_gripper_efforts() override {
      return self->read_gripper_effort();
    }

    /** @brief Does nothing */
    void apply_multiple_gripper_feedback(std::vector<float> follower_gripper_effort) override {}
  };

  struct CartView : teleop::CartesianSpaceTeleop {
    RivetComponent* self;
    explicit CartView(RivetComponent* s) : self(s) {}
    /**
     * @brief Read the arm cartesian positions, base velocities, and lift velocities
     * @return Vector (18) [left_x, left_y, left_z, left_rx, left_ry, left_rz, left_gripper_m,
     *         right_x, right_y, right_z, right_rx, right_ry, right_rz, right_gripper_m,
     *         base_vx, base_vy, base_vr, base_vlift]
     */
    std::vector<float> read() override {
      return self->read_cartesian();
    }
    /** @brief Write arm cartesian positions, base velocities, and lift velocities
     *  @param cmd Vector (18)  [left_x, left_y, left_z, left_rx, left_ry, left_rz, left_gripper_m
     *         right_x, right_y, right_z, right_rx, right_ry, right_rz, right_gripper_m
     *         base_vx, base_vy, base_vr, base_vlift]
    */
    void write(const std::vector<float>& cmd) override {
      self->write_cartesian(cmd);
    }
  };

  JointView joint_view_{this};
  CartView  cart_view_{this};


  std::shared_ptr<trossen_arm::TrossenArmDriver> left_driver_;
  std::shared_ptr<trossen_arm::TrossenArmDriver> right_driver_;
  std::shared_ptr<trossen_base::TrossenBase> base_driver_;

  /// @brief Rate at which base_update_loop() ticks base_driver_->update_base()
  static constexpr double BASE_UPDATE_HZ = 15.0;

  /// @brief Dedicated thread running base_update_loop()
  std::thread base_update_thread_;

  /// @brief Running flag for base_update_thread_ (atomic for thread-safe stop)
  std::atomic<bool> base_update_running_{false};

  std::string left_model_str_;
  std::string left_ip_address_;
  std::string right_model_str_;
  std::string right_ip_address_;
  std::string end_effector_str_;
  size_t njoints_;  // Joints per arm

  // TODO: @schromya complete/check
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

  // Hack for exposing data (@schromya update once trossen_base exposes these)
  double last_base_vx_{0.0};
  double last_base_vy_{0.0};
  double last_base_vr_{0.0};
  double last_base_lift_{0.0};
};

}  // namespace trossen::hw::rivet

#endif  // TROSSEN_SDK__HW__ARM__RIVET_COMPONENT_HPP_
