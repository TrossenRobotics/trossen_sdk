/**
 * @file vr_base_joystick.hpp
 * @brief VR thumbstick as a Base-space (linear, angular) teleop leader.
 */

#ifndef TROSSEN_SDK__HW__VR__VR_BASE_JOYSTICK_HPP_
#define TROSSEN_SDK__HW__VR__VR_BASE_JOYSTICK_HPP_

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"

namespace trossen::hw::vr {

/**
 * @brief Virtual base-velocity leader driven by Meta Quest thumbstick(s).
 *
 * Each `read()` samples the configured thumbstick(s) and returns a
 * 2-element vector `[linear_mps, angular_rps]`:
 *  - `<linear_controller>_thumbstick_y`  → forward/backward linear velocity.
 *  - `<angular_controller>_thumbstick_x` → yaw rate (sign negated so that
 *    pushing the stick left yields a positive yaw rate, matching the
 *    right-hand rule around the vertical axis).
 *
 * The two axes can come from the same or different controllers.
 * When different (e.g. right thumbstick for linear, left for yaw), the
 * component claims the thumbstick input on both hands.
 *
 * Values below `deadzone` (in absolute magnitude) are clamped to zero;
 * above `deadzone` they are rescaled so the effective travel starts at
 * zero at the deadzone boundary and reaches ±1 at the stick limit, then
 * multiplied by `max_linear_mps` / `max_angular_rps`.
 *
 * The component is a leader only: `write()` is a no-op. It shares the
 * process-wide VrSession with the VR arm controller(s), so one WebSocket
 * connection feeds arm + base teleop together.
 */
class VrBaseJoystickComponent : public HardwareComponent,
                                public teleop::BaseSpaceTeleop {
public:
  explicit VrBaseJoystickComponent(std::string identifier)
      : HardwareComponent(std::move(identifier)) {}

  ~VrBaseJoystickComponent() override;

  VrBaseJoystickComponent(const VrBaseJoystickComponent&)            = delete;
  VrBaseJoystickComponent& operator=(const VrBaseJoystickComponent&) = delete;
  VrBaseJoystickComponent(VrBaseJoystickComponent&&)                 = delete;
  VrBaseJoystickComponent& operator=(VrBaseJoystickComponent&&)      = delete;

  /**
   * @brief Configure from JSON.
   *
   * Required (one of):
   *   - `controller` : "left" or "right" — single-hand mode. Both linear
   *                    and angular axes come from this controller's
   *                    thumbstick. Kept for backward compatibility.
   *   - `linear_controller` + `angular_controller` : per-axis split
   *                    mode. Each may independently be "left" or
   *                    "right"; they can be the same (equivalent to
   *                    the single-hand form) or different (e.g. right
   *                    thumbstick for linear, left for yaw).
   *
   * Optional:
   *   - `vr_port`          : WebSocket port (default 5432).
   *   - `max_linear_mps`   : Stick-at-limit linear velocity (default 0.5 m/s).
   *   - `max_angular_rps`  : Stick-at-limit yaw rate (default 1.0 rad/s).
   *   - `deadzone`         : Centered magnitude below which the stick reads
   *                         zero (default 0.1, applied to the raw -1..1).
   *   - `wait_for_quest_s` : How long `prepare_for_teleop()` waits for the
   *                         Quest app to connect before throwing (default 10 s).
   */
  void configure(const nlohmann::json& config) override;

  std::string    get_type() const override { return "vr_base_joystick"; }
  nlohmann::json get_info() const override;

  // ── teleop::BaseSpaceTeleop: IO contract ─────────────────────────────────

  /// Sample the thumbstick and return `[linear_mps, angular_rps]`. Returns
  /// zeros while the Quest is disconnected or no frame has arrived.
  std::vector<float> read() override;

  /// Leader role: no-op.
  void write(const std::vector<float>& cmd) override;

  // ── teleop::TeleopCapable: lifecycle ────────────────────────────────────

  void prepare_for_teleop() override;
  void end_teleop() override;

private:
  /// Rescale `v` from `[-1, 1]` to `[-1, 1]` with a centered deadzone applied.
  static double apply_deadzone(double v, double deadzone);

  /// Config — per-axis hand assignment. Set identically (both "left"
  /// or both "right") for single-hand mode; set differently for a
  /// split layout (e.g. right-stick linear, left-stick yaw).
  std::string   linear_controller_{"left"};
  std::string   angular_controller_{"left"};
  std::uint16_t vr_port_{5432};
  double        max_linear_mps_{0.5};
  double        max_angular_rps_{1.0};
  double        deadzone_{0.1};
  std::chrono::milliseconds wait_for_quest_{std::chrono::seconds{10}};

  bool session_held_{false};
};

}  // namespace trossen::hw::vr

#endif  // TROSSEN_SDK__HW__VR__VR_BASE_JOYSTICK_HPP_
