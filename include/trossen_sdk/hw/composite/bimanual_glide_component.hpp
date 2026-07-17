/**
 * @file trossen_arm_component.hpp
 * @brief Hardware component wrapper for Trossen Robotics arms.
 */

#ifndef TROSSEN_SDK__HW__ARM__BIMANUAL_GLIDE_COMPONENT_HPP_
#define TROSSEN_SDK__HW__ARM__BIMANUAL_GLIDE_COMPONENT_HPP_

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
   * Expected JSON format ):
   * {
   *   "left_ip_address": "192.168.1.100",
   *   "left_model": "glide_right",
   *   "right_ip_address": "192.168.1.101",
   *   "right_model: "glide_left",
   *   "write_moving_time_s": 0.2
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

private:
  // Space-specific IO helpers. Called by the nested adapter views.
  std::vector<float> read_joint();
  void               write_joint(const std::vector<float>& cmd);

  std::vector<float> read_cartesian();
  void               write_cartesian(const std::vector<float>& cmd);

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

    /**
     * @brief Write joint commands to both arms
     * @param cmd Vector of joint efforts for gripper [left_q6, right_q6]
     */
    void write(const std::vector<float>& cmd) override {
      self->write_joint(cmd);
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
    /** DOes nothing */
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
  double write_moving_time_s_{0.0};

  /// Whether this arm participates in the per-episode lifecycle (staging before
  /// each episode). Opt-in; parsed from "episode_lifecycle_enabled" in configure().
  bool episode_lifecycle_enabled_{false};

};

}  // namespace trossen::hw::bimanual_glide

#endif  // TROSSEN_SDK__HW__ARM__BIMANUAL_GLIDE_COMPONENT_HPP_
