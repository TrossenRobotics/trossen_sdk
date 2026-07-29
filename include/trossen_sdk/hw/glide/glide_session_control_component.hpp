/**
 * @file glide_session_control_component.hpp
 * @brief Session control driven by Glide handle buttons.
 */

#ifndef TROSSEN_SDK__HW__GLIDE__GLIDE_SESSION_CONTROL_COMPONENT_HPP_
#define TROSSEN_SDK__HW__GLIDE__GLIDE_SESSION_CONTROL_COMPONENT_HPP_

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

#include "trossen_sdk/hw/glide/glide_session.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/session_control/session_control_capable.hpp"

namespace trossen::hw::glide {

/**
 * @brief Turns Glide handle button presses into session-control intents.
 *
 * The operator teleoperating a Rivet has both hands on the Glide handles, so
 * reaching for a keyboard to start or re-record an episode means letting go of
 * the robot mid-task. This component maps handle buttons onto the same
 * `SessionControlEvent` intents the keyboard source emits, which is what lets
 * the cockpit display be a read-only heads-up view rather than something the
 * operator has to drive.
 *
 * Push-based, per the `SessionControlCapable` contract: a small poller thread
 * watches the claimed buttons and emits on the rising edge.
 *
 * Expected JSON:
 * @code
 * {
 *   "buttons": [
 *     { "arm_id": "glide_right", "bit": 1, "event": "start" },
 *     { "arm_id": "glide_right", "bit": 3, "event": "rerecord" },
 *     { "arm_id": "glide_left",  "bit": 1, "event": "stop_early" },
 *     { "arm_id": "glide_left",  "bit": 3, "event": "stop_session" }
 *   ],
 *   "poll_rate_hz": 50.0,
 *   "debounce_ms": 40
 * }
 * @endcode
 *
 * Event names match `SessionControlEvent`: `start`, `stop_early`, `rerecord`,
 * `stop_session`. Bit assignments are configuration because the handle's button
 * bitmask layout is not documented anywhere the SDK can see it — see
 * `GlideBaseComponent` for the same reasoning.
 */
class GlideSessionControlComponent
  : public HardwareComponent,
    public session_control::SessionControlCapable {
public:
  explicit GlideSessionControlComponent(std::string identifier)
    : HardwareComponent(std::move(identifier)) {}

  /// Stops the poller before the claim lease releases, so the thread cannot
  /// read a snapshot for an input this component no longer holds.
  ~GlideSessionControlComponent() override;

  GlideSessionControlComponent(const GlideSessionControlComponent&)            = delete;
  GlideSessionControlComponent& operator=(const GlideSessionControlComponent&) = delete;
  GlideSessionControlComponent(GlideSessionControlComponent&&)                 = delete;
  GlideSessionControlComponent& operator=(GlideSessionControlComponent&&)      = delete;

  /**
   * @brief Parse the button map and claim each button.
   *
   * @throws std::invalid_argument on an unknown event name, a missing field, or
   *         a non-positive poll rate.
   * @throws std::runtime_error if a button is already claimed elsewhere.
   */
  void configure(const nlohmann::json& config) override;

  std::string get_type() const override { return "glide_session_control"; }

  nlohmann::json get_info() const override;

  // ── SessionControlCapable ────────────────────────────────────────────────

  /// Start the poller thread. Idempotent.
  void start() override;

  /// Stop and join the poller thread. Idempotent, and safe from the
  /// destructor.
  void stop() override;

  /**
   * @brief Evaluate one polling step against the current snapshots.
   *
   * Exposed so tests can drive edge detection deterministically — inject a
   * snapshot via `GlideSession::set_test_snapshot()`, call this, and assert on
   * what was emitted, with no thread and no sleeping. `start()` simply calls it
   * on a timer.
   */
  void poll_once();

private:
  /// One button-to-intent binding.
  struct ButtonBinding {
    std::string                            arm_id;
    int                                    bit{-1};
    session_control::SessionControlEvent   event{
      session_control::SessionControlEvent::kNone};

    /// Last observed state, for rising-edge detection. A held button emits
    /// once, not once per poll.
    bool was_pressed{false};

    /// When the current reading first differed from `was_pressed`; a change
    /// must persist through the debounce window before it counts.
    std::chrono::steady_clock::time_point changed_at{};
    bool pending{false};
  };

  void poll_loop();

  std::vector<ButtonBinding> buttons_;
  double                     poll_rate_hz_{50.0};
  std::chrono::milliseconds  debounce_{std::chrono::milliseconds(40)};

  std::thread       poll_thread_;
  std::atomic<bool> running_{false};

  GlideClaimLease lease_;
};

}  // namespace trossen::hw::glide

#endif  // TROSSEN_SDK__HW__GLIDE__GLIDE_SESSION_CONTROL_COMPONENT_HPP_
