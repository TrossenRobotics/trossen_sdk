/**
 * @file vr_session_control.hpp
 * @brief Meta Quest buttons as a session-control source.
 *
 * Third VR hardware component in the stack. Where
 * `VrArmControllerComponent` claims pose + trigger and
 * `VrBaseJoystickComponent` claims the thumbstick,
 * `VrSessionControlComponent` claims the buttons (A, B, grip, menu)
 * and pushes session-level intents — kStart / kRerecord / kStopSession
 * — to SessionManager via the `SessionControlCapable` mixin.
 *
 * The three components share one `VrSession` connection and each
 * declares its inputs through `VrSession::claim_inputs()`, so a typo
 * that tries to, e.g., use the trigger for session control on the
 * same hand as an arm controller fails at configure() time rather
 * than silently misbehaving.
 */

#ifndef TROSSEN_SDK__HW__VR__VR_SESSION_CONTROL_HPP_
#define TROSSEN_SDK__HW__VR__VR_SESSION_CONTROL_HPP_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/session_control/session_control_capable.hpp"
#include "trossen_sdk/hw/vr/vr_session.hpp"

namespace trossen::hw::vr {

/**
 * @brief VR button component that drives SessionManager state transitions.
 *
 * Leader-only: no teleop role. The component's reader thread polls the
 * shared VR frame stream, detects rising edges on each bound button,
 * and fires `SessionControlCapable` event callbacks accordingly.
 * Disconnection is detected via frame-sequence staleness: if the VR
 * app stops delivering new frames for longer than the configured
 * threshold after having been connected, the disconnect callback
 * fires exactly once so SessionManager halts the session.
 */
class VrSessionControlComponent
  : public HardwareComponent,
    public session_control::SessionControlCapable {
public:
  explicit VrSessionControlComponent(std::string identifier)
      : HardwareComponent(std::move(identifier)) {}

  ~VrSessionControlComponent() override;

  VrSessionControlComponent(const VrSessionControlComponent&)            = delete;
  VrSessionControlComponent& operator=(const VrSessionControlComponent&) = delete;
  VrSessionControlComponent(VrSessionControlComponent&&)                 = delete;
  VrSessionControlComponent& operator=(VrSessionControlComponent&&)      = delete;

  /**
   * @brief Configure from JSON.
   *
   * Required:
   *   - `controller`: "left" or "right".
   *
   * Optional:
   *   - `bindings`: object mapping VR input name to event name. Defaults:
   *     `{ "button_a": "start", "button_b": "rerecord",
   *        "grip": "stop_session" }`. Recognized input names:
   *     "button_a", "button_b", "menu", "trigger", "grip".
   *     Recognized event names: "start", "stop_early", "rerecord",
   *     "stop_session".
   *   - `vr_port`: WebSocket port (default 5432).
   *   - `wait_for_quest_s`: How long `start()` blocks for the Quest app
   *     to connect before throwing (default 10 s).
   *   - `poll_interval_ms`: Reader-thread poll cadence (default 50 ms).
   *   - `analog_threshold`: Fraction in (0, 1) above which an analog
   *     input (trigger, grip) is considered pressed (default 0.5).
   *   - `disconnect_timeout_s`: Consecutive seconds without a new
   *     frame sequence before disconnect fires (default 2 s).
   *
   * Claims the inputs referenced by `bindings` on the configured hand
   * so overlapping VR configurations fail loudly at configure() time.
   */
  void configure(const nlohmann::json& config) override;

  std::string    get_type() const override { return "vr_session_control"; }
  nlohmann::json get_info() const override;

  // ── SessionControlCapable ────────────────────────────────────────────

  void set_callbacks(EventCallback on_event,
                     DisconnectCallback on_disconnect) override;
  void start() override;
  void stop() override;

  /// Public so the in-cpp default-bindings helper can name it; still a
  /// detail of this component.
  struct Binding {
    VrInput                                input;
    session_control::SessionControlEvent   event;
  };

private:
  /// Reader-thread loop: samples the VR frame stream and emits events.
  void reader_loop();

  /// Ordered list of button-map keys to probe for a given input on
  /// the configured hand — first match wins in the frame's button
  /// map. The shipping Meta Quest app sends A/B as bare keys
  /// (`"a"`, `"b"`) while grip/trigger are always per-hand
  /// (`"right_grip"`), so we have to try both shapes.
  std::vector<std::string> input_to_keys(VrInput input) const;

  /// True if an input is a digital button (bool in the protocol).
  /// False for analog inputs (trigger, grip) that use a threshold.
  static bool is_digital(VrInput input);

  std::vector<Binding> bindings_;

  /// Config primitives.
  std::string               controller_{"right"};
  std::uint16_t             vr_port_{5432};
  std::chrono::milliseconds wait_for_quest_{std::chrono::seconds{10}};
  std::chrono::milliseconds poll_interval_{std::chrono::milliseconds{50}};
  double                    analog_threshold_{0.5};
  std::chrono::milliseconds disconnect_timeout_{std::chrono::seconds{2}};

  /// VR session refcount: acquired in configure(), released in dtor.
  bool session_held_{false};

  /// Callbacks installed by SessionManager via `set_callbacks()`.
  EventCallback      event_cb_;
  DisconnectCallback disconnect_cb_;

  /// Reader-thread state. `stop_requested_` ends the loop; `running_`
  /// guards start/stop idempotency.
  std::thread              reader_thread_;
  std::atomic<bool>        stop_requested_{false};
  std::atomic<bool>        running_{false};

  /// Rising-edge state per bound input.
  std::unordered_map<VrInput, bool> prev_pressed_;
};

}  // namespace trossen::hw::vr

#endif  // TROSSEN_SDK__HW__VR__VR_SESSION_CONTROL_HPP_
