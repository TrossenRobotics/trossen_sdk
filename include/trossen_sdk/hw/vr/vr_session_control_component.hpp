/**
 * @file vr_session_control_component.hpp
 * @brief VR controller buttons as a session-control source.
 */

#ifndef TROSSEN_SDK__HW__VR__VR_SESSION_CONTROL_COMPONENT_HPP_
#define TROSSEN_SDK__HW__VR__VR_SESSION_CONTROL_COMPONENT_HPP_

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
 * Polls the shared VR frame stream on a background thread, detects rising
 * edges on bound buttons, and fires `SessionControlCapable` event callbacks.
 * Disconnection is detected when frames stop arriving for longer than the
 * configured threshold. Input conflicts (e.g. two components claiming the
 * same button on the same controller side) are caught at `configure()` time.
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
   *   - `controller_type`: "left" or "right".
   *
   * Optional:
   *   - `bindings`: object mapping VR input name to event name. Defaults:
   *     `{ "button_a": "start", "button_b": "rerecord" }`.
   *     Recognized input names:
   *       "button_a" / "button_x" — primary button (A on right, X on left).
   *       "button_b" / "button_y" — secondary button (B on right, Y on left).
   *     Recognized event names: "start", "stop_early", "rerecord", "stop_session".
   *   - `vr_port`: network port (default 9000).
   *   - `connection_timeout_s`: How long `start()` blocks for the headset
   *     app to connect before throwing (default 10 s).
   *   - `poll_interval_ms`: reader-thread poll cadence (default 50 ms).
   *   - `disconnect_timeout_s`: consecutive seconds without a new
   *     frame before disconnect fires (default 2 s).
   *
   * Claims the inputs referenced by `bindings` on the configured controller
   * type so overlapping VR configurations fail loudly at configure() time.
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

  std::vector<Binding> bindings_;

  /// Config primitives.
  std::string               controller_type_{"right"};
  std::uint16_t             vr_port_{9000};
  std::chrono::milliseconds connection_timeout_{std::chrono::seconds{10}};
  std::chrono::milliseconds poll_interval_{std::chrono::milliseconds{50}};
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

#endif  // TROSSEN_SDK__HW__VR__VR_SESSION_CONTROL_COMPONENT_HPP_
