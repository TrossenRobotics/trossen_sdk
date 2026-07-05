/**
 * @file vr_base_component.hpp
 * @brief VR thumbstick as a base-velocity (linear, angular) teleop leader.
 */

#ifndef TROSSEN_SDK__HW__VR__VR_BASE_COMPONENT_HPP_
#define TROSSEN_SDK__HW__VR__VR_BASE_COMPONENT_HPP_

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"
#include "trossen_sdk/hw/vr/vr_session.hpp"

namespace trossen::hw::vr {

/**
 * @brief Base-velocity teleop leader driven by VR controller thumbstick(s).
 *
 * Each `read()` samples the configured thumbstick axis/axes and returns
 * `[linear_mps, angular_rps]`. Values below `deadzone` are zeroed; above
 * it they are rescaled to span the full range and then multiplied by
 * `max_linear_mps` / `max_angular_rps`. `write()` is a no-op (leader only).
 * Shares the process-wide `VrSession` with all other VR hardware components.
 *
 * The two axes can come from the same or different controllers (split mode).
 * In split mode the component claims the thumbstick input on both sides.
 */
class VrBaseComponent : public HardwareComponent,
                                public teleop::BaseSpaceTeleop {
public:
  explicit VrBaseComponent(std::string identifier)
      : HardwareComponent(std::move(identifier)) {}

  ~VrBaseComponent() override = default;

  VrBaseComponent(const VrBaseComponent&)            = delete;
  VrBaseComponent& operator=(const VrBaseComponent&) = delete;
  VrBaseComponent(VrBaseComponent&&)                 = delete;
  VrBaseComponent& operator=(VrBaseComponent&&)      = delete;

  /**
   * @brief Configure from JSON.
   *
   * Required (one of):
   *   - `controller_type` : "left" or "right" — single-hand mode. Both the
   *                    linear and angular axes come from this controller's
   *                    thumbstick (Y-axis → forward/backward, X-axis → yaw).
   *   - `linear_controller_type` and/or `angular_controller_type` : per-axis
   *                    split mode. Either may be given independently; a missing
   *                    axis falls back to `controller_type` if present, else
   *                    "left". Each is "left" or "right". The two axes may use
   *                    the same controller (equivalent to single-hand mode) or
   *                    different ones — e.g. right thumbstick for driving
   *                    forward, left thumbstick for turning.
   *
   * Output of `read()` is always `[linear_mps, angular_rps]`:
   *   - `linear_mps`  : forward velocity (positive = forward). Driven by the
   *                    Y-axis of `linear_controller_type`'s thumbstick.
   *   - `angular_rps` : yaw rate (positive = left/CCW, right-hand rule about
   *                    the vertical axis). Driven by the X-axis of
   *                    `angular_controller_type`'s thumbstick with sign
   *                    negated so pushing left → positive yaw.
   *
   * Optional:
   *   - `vr_port`              : network port (default 9000).
   *   - `max_linear_mps`       : Speed at full stick deflection (default 0.5 m/s).
   *   - `max_angular_rps`      : Yaw rate at full stick deflection (default 1.0 rad/s).
   *   - `deadzone`             : Stick magnitude below which output is zero,
   *                             applied to the raw −1..1 range (default 0.1).
   *                             Above the deadzone the output is rescaled so
   *                             full deflection always reaches ±max.
   *   - `connection_timeout_s` : How long `prepare_for_teleop()` waits for the
   *                             headset to connect before throwing (default 10 s).
   *
   * @throws std::runtime_error if required fields are missing or invalid.
   */
  void configure(const nlohmann::json& config) override;

  std::string    get_type() const override { return "vr_base_component"; }
  nlohmann::json get_info() const override;

  // ── teleop::BaseSpaceTeleop: IO contract ─────────────────────────────────

  /// Sample the thumbstick and return `[linear_mps, angular_rps]`. Returns
  /// zeros while the headset is disconnected or no frame has arrived.
  std::vector<float> read() override;

  /// Leader role: no-op.
  void write(const std::vector<float>& cmd) override;

  // ── teleop::TeleopCapable: lifecycle ────────────────────────────────────

  void prepare_for_teleop() override;
  void end_teleop() override;

private:
  /// Config — per-axis controller-type assignment.
  std::string   linear_controller_type_{"left"};
  std::string   angular_controller_type_{"left"};
  std::uint16_t vr_port_{9000};
  double        max_linear_mps_{0.5};
  double        max_angular_rps_{1.0};
  double        deadzone_{0.1};
  std::chrono::milliseconds connection_timeout_{std::chrono::seconds{10}};

  /// Holds this component's reference on the shared VrSession; releases the
  /// reference and input claims automatically on teardown.
  VrSessionLease session_lease_;
};

}  // namespace trossen::hw::vr

#endif  // TROSSEN_SDK__HW__VR__VR_BASE_COMPONENT_HPP_
