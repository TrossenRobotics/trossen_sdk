/**
 * @file vr_arm_controller.hpp
 * @brief VR controller (hand) as a Cartesian-space teleop leader.
 */

#ifndef TROSSEN_SDK__HW__VR__VR_ARM_CONTROLLER_HPP_
#define TROSSEN_SDK__HW__VR__VR_ARM_CONTROLLER_HPP_

#include <Eigen/Dense>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"

namespace trossen::hw::vr {

/**
 * @brief Virtual cartesian leader driven by a Meta Quest controller.
 *
 * Emits a 7-element vector `[x, y, z, rx, ry, rz, gripper_m]` on each `read()`:
 *  - The first 6 elements are the follower's target pose in the robot base
 *    frame, computed as `T_offset * T_vr_now` where `T_offset` was captured
 *    in `sync_to_state()` from the follower's pose at the moment teleop
 *    started. This anchors the VR controller's position/orientation to the
 *    robot's current pose so the first mirror tick does not snap the arm.
 *  - The 7th element is the gripper opening in meters, derived by linearly
 *    mapping the controller trigger (0..1) onto `[gripper_min_m, gripper_max_m]`.
 *
 * The component is a leader only: `write()` is a no-op. It shares a single
 * VrSession with every other VR hardware component in the process, so two
 * instances (one per hand) use one underlying WebSocket connection.
 */
class VrArmControllerComponent : public HardwareComponent,
                                 public teleop::CartesianSpaceTeleop {
public:
  explicit VrArmControllerComponent(std::string identifier)
      : HardwareComponent(std::move(identifier)) {}

  ~VrArmControllerComponent() override;

  VrArmControllerComponent(const VrArmControllerComponent&)            = delete;
  VrArmControllerComponent& operator=(const VrArmControllerComponent&) = delete;
  VrArmControllerComponent(VrArmControllerComponent&&)                 = delete;
  VrArmControllerComponent& operator=(VrArmControllerComponent&&)      = delete;

  /**
   * @brief Configure from JSON.
   *
   * Required:
   *   - `controller` : "left" or "right" — which VR hand this component mirrors.
   *
   * Optional:
   *   - `vr_port`          : WebSocket port (default 5432).
   *   - `gripper_min_m`    : Gripper opening at trigger=0 (default 0.0 m).
   *   - `gripper_max_m`    : Gripper opening at trigger=1 (default 0.04 m).
   *   - `wait_for_quest_s` : How long `prepare_for_teleop()` waits for the
   *                         Quest app to connect before throwing (default 10 s).
   *
   * @throws std::runtime_error if required fields are missing or invalid.
   */
  void configure(const nlohmann::json& config) override;

  std::string get_type() const override { return "vr_arm_controller"; }
  nlohmann::json get_info() const override;

  // ── teleop::CartesianSpaceTeleop: IO contract ────────────────────────────

  /// Return the latest mapped robot-frame 7-vec. Returns the last good
  /// sample (or all-zeros) while the Quest is disconnected or no frame has
  /// arrived yet; the controller swallows transient drops without stuttering.
  std::vector<float> read() override;

  /// Leader role: no-op. The controller only reads from this component.
  void write(const std::vector<float>& cmd) override;

  /// Capture the VR-to-robot alignment transform.
  ///
  /// `state` must be the follower's current 7-vec cartesian state. The
  /// component combines it with the current VR pose to produce `T_offset`;
  /// subsequent `read()` calls use that offset until teleop ends.
  void sync_to_state(const std::vector<float>& state) override;

  // ── teleop::TeleopCapable: lifecycle ────────────────────────────────────

  /// Block until the Quest app connects or `wait_for_quest_s` elapses.
  void prepare_for_teleop() override;

  /// Release this component's reference on the shared VrSession.
  void end_teleop() override;

private:
  /// Extract the configured controller's pose from the latest VR frame,
  /// as a 6-vec axis-angle. Returns nullopt if the frame or pose is missing.
  std::optional<std::array<double, 6>> read_vr_pose() const;

  /// Extract the configured controller's trigger value from the latest frame.
  /// Returns 0.0 if the frame or the button is missing or not analog.
  double read_trigger() const;

  /// Config
  std::string   controller_{"right"};
  std::uint16_t vr_port_{5432};
  double        gripper_min_m_{0.0};
  double        gripper_max_m_{0.04};
  std::chrono::milliseconds wait_for_quest_{std::chrono::seconds{10}};

  /// Session bookkeeping — guards against a double-release from the
  /// destructor when `end_teleop()` already released.
  bool session_held_{false};

  /// `sync_to_state()` sets this; `read()` only transforms once initialized.
  bool              initialized_{false};
  Eigen::Matrix4d   t_offset_{Eigen::Matrix4d::Identity()};

  /// Last successfully produced 7-vec, used to ride through dropouts.
  std::vector<float> last_good_{std::vector<float>(7, 0.0f)};
};

}  // namespace trossen::hw::vr

#endif  // TROSSEN_SDK__HW__VR__VR_ARM_CONTROLLER_HPP_
