/**
 * @file keyboard_component.hpp
 * @brief Keyboard as a session-control hardware component.
 *
 * The keyboard is modeled as a first-class HardwareComponent that
 * implements `SessionControlCapable`, so the same attach/detach API
 * used for VR controllers, foot pedals, etc. also covers the terminal.
 * It owns its own reader thread (blocking `poll()` on stdin) and
 * manages the terminal's raw-mode transitions internally — neither
 * responsibility belongs on SessionManager.
 *
 * Default bindings mirror the historical in-SessionManager behavior:
 *   left arrow  → kRerecord
 *   right arrow → kStart
 *   q / Q       → kStopSession
 * Bindings can be overridden per component via JSON config.
 */

#ifndef TROSSEN_SDK__HW__KEYBOARD__KEYBOARD_COMPONENT_HPP_
#define TROSSEN_SDK__HW__KEYBOARD__KEYBOARD_COMPONENT_HPP_

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/session_control/session_control_capable.hpp"
#include "trossen_sdk/utils/keyboard_input_utils.hpp"

namespace trossen::hw::keyboard {

/**
 * @brief Terminal keyboard session-control source.
 *
 * Leader-only: does not teleop anything. `read()` is not exposed; the
 * component talks to SessionManager through the `SessionControlCapable`
 * callbacks.
 */
class KeyboardComponent : public HardwareComponent,
                          public session_control::SessionControlCapable {
public:
  explicit KeyboardComponent(std::string identifier)
      : HardwareComponent(std::move(identifier)) {}

  ~KeyboardComponent() override;

  KeyboardComponent(const KeyboardComponent&)            = delete;
  KeyboardComponent& operator=(const KeyboardComponent&) = delete;
  KeyboardComponent(KeyboardComponent&&)                 = delete;
  KeyboardComponent& operator=(KeyboardComponent&&)      = delete;

  /**
   * @brief Configure from JSON.
   *
   * Optional:
   *   - `bindings`: object mapping key name to event name, e.g.
   *     `{ "left_arrow": "rerecord", "right_arrow": "start",
   *        "q": "stop_session" }`. Unmapped keys are ignored. When the
   *     field is absent the defaults above apply.
   *   - `poll_interval_ms`: how long the reader thread blocks per poll
   *     iteration (default 100 ms). Lower values shorten shutdown
   *     latency; higher values reduce wakeups.
   *
   * Recognized key names: "left_arrow", "right_arrow", "up_arrow",
   * "down_arrow", "space", "enter", "q".
   * Recognized event names: "start", "stop_early", "rerecord",
   * "stop_session".
   */
  void configure(const nlohmann::json& config) override;

  std::string    get_type() const override { return "keyboard"; }
  nlohmann::json get_info() const override;

  // ── SessionControlCapable ────────────────────────────────────────────

  void set_callbacks(EventCallback on_event,
                     DisconnectCallback on_disconnect) override;
  void start() override;
  void stop() override;

private:
  /// Reader-thread loop: polls stdin, translates keypresses to events,
  /// and fires the event callback on matches.
  void reader_loop();

  /// Config
  std::unordered_map<utils::KeyPress, session_control::SessionControlEvent>
    bindings_;
  std::chrono::milliseconds poll_interval_{std::chrono::milliseconds{100}};

  /// Lifecycle
  EventCallback                     event_cb_;
  DisconnectCallback                disconnect_cb_;
  std::unique_ptr<utils::RawModeGuard> raw_mode_;
  std::thread                       reader_thread_;
  std::atomic<bool>                 stop_requested_{false};
  std::atomic<bool>                 running_{false};
};

}  // namespace trossen::hw::keyboard

#endif  // TROSSEN_SDK__HW__KEYBOARD__KEYBOARD_COMPONENT_HPP_
