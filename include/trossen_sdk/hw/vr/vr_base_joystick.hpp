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
 * @brief Virtual base-velocity leader driven by a Meta Quest thumbstick.
 *
 * Each `read()` samples the configured thumbstick and returns a 2-element
 * vector `[linear_mps, angular_rps]`:
 *  - `<controller>_thumbstick_y` maps to forward/backward linear velocity.
 *  - `<controller>_thumbstick_x` maps to yaw rate (with the sign negated so
 *    that pushing the stick left yields a positive yaw rate, matching the
 *    right-hand rule around the vertical axis).
 *
 * Values below `deadzone` (in absolute magnitude) are clamped to zero; above
 * `deadzone` they are rescaled so the effective travel starts at zero at the
 * deadzone boundary and reaches ±1 at the stick limit, then multiplied by
 * `max_linear_mps` / `max_angular_rps`.
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
   * Required:
   *   - `controller` : "left" or "right" — which thumbstick to read.
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

  /// Config
  std::string   controller_{"left"};
  std::uint16_t vr_port_{5432};
  double        max_linear_mps_{0.5};
  double        max_angular_rps_{1.0};
  double        deadzone_{0.1};
  std::chrono::milliseconds wait_for_quest_{std::chrono::seconds{10}};

  bool session_held_{false};
};

}  // namespace trossen::hw::vr

#endif  // TROSSEN_SDK__HW__VR__VR_BASE_JOYSTICK_HPP_
