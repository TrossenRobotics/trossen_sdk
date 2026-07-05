/**
 * @file vr_arm_component.hpp
 * @brief VR controller as a Cartesian-space teleop leader.
 */

#ifndef TROSSEN_SDK__HW__VR__VR_ARM_COMPONENT_HPP_
#define TROSSEN_SDK__HW__VR__VR_ARM_COMPONENT_HPP_

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "trossen_vr/vr_types.hpp"

#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"
#include "trossen_sdk/hw/vr/vr_session.hpp"

namespace trossen::hw::vr {

/**
 * @brief Cartesian-space teleop leader driven by a VR controller.
 *
 * Each `read()` applies a frozen pose offset (captured in `sync_to_state()`)
 * to the live controller pose, producing a 7-element `[x, y, z, rx, ry, rz,
 * gripper_m]` target for the follower arm. `write()` is a no-op (leader only).
 * Shares the process-wide `VrSession` with all other VR hardware components.
 */
class VrArmComponent : public HardwareComponent,
                                 public teleop::CartesianSpaceTeleop {
public:
  explicit VrArmComponent(std::string identifier)
      : HardwareComponent(std::move(identifier)) {}

  ~VrArmComponent() override = default;

  VrArmComponent(const VrArmComponent&)            = delete;
  VrArmComponent& operator=(const VrArmComponent&) = delete;
  VrArmComponent(VrArmComponent&&)                 = delete;
  VrArmComponent& operator=(VrArmComponent&&)      = delete;

  /**
   * @brief Configure from JSON.
   *
   * Required:
   *   - `controller_type`      : "left" or "right" — which VR controller this component mirrors.
   *
   * Optional:
   *   - `vr_port`              : network port (default 9000).
   *   - `gripper_min_m`        : Gripper opening at trigger=0 (default 0.0 m).
   *   - `gripper_max_m`        : Gripper opening at trigger=1 (default 0.04 m).
   *   - `connection_timeout_s` : How long `prepare_for_teleop()` waits for the
   *                             headset to connect before throwing (default 10 s).
   *
   * @throws std::runtime_error if required fields are missing or invalid.
   */
  void configure(const nlohmann::json& config) override;

  std::string get_type() const override { return "vr_arm_component"; }
  nlohmann::json get_info() const override;

  // ── teleop::TeleopCapable: lifecycle ────────────────────────────────────

  /// Block until the VR headset connects or `connection_timeout_s` elapses.
  void prepare_for_teleop() override;

  /// Release this component's reference on the shared VrSession.
  void end_teleop() override;

  // ── teleop::CartesianSpaceTeleop: IO contract ────────────────────────────

  /// Return the latest mapped robot-frame 7-vec. Returns the last good
  /// sample (or all-zeros) while the headset is disconnected or no frame has
  /// arrived yet; holds position through transient drops without stuttering.
  std::vector<float> read() override;

  /// Leader role: no-op.
  void write(const std::vector<float>& cmd) override;

  /// Capture the VR-to-robot alignment transform.
  ///
  /// Called once by the teleop controller before the mirror loop starts,
  /// with the follower's current cartesian state. Computes `T_offset` so
  /// the first `read()` tick produces the follower's current pose, not
  /// a snap to the VR controller's absolute world position.
  void sync_to_state(const std::vector<float>& state) override;

private:
  /// Config
  std::string   controller_type_{"right"};
  std::uint16_t vr_port_{9000};
  double        gripper_min_m_{0.0};
  double        gripper_max_m_{0.04};
  std::chrono::milliseconds connection_timeout_{std::chrono::seconds{10}};

  /// Holds this component's reference on the shared VrSession; releases the
  /// reference and input claims automatically on teardown.
  VrSessionLease session_lease_;

  /// `sync_to_state()` sets this; `read()` only transforms once initialized.
  bool                       initialized_{false};
  trossen_vr::Transform4D    t_offset_{};

  /// Track previous is_tracked state to detect hand-grip deadman transitions.
  /// When is_tracked goes from 0→1, we recalculate offset.
  bool              prev_tracked_{false};

  /// Last successfully produced 7-vec, used to ride through dropouts.
  std::vector<float> last_good_{std::vector<float>(7, 0.0f)};
};

}  // namespace trossen::hw::vr

#endif  // TROSSEN_SDK__HW__VR__VR_ARM_COMPONENT_HPP_
