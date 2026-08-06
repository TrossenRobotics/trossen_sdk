/**
 * @file session_control_capable.hpp
 * @brief Mixin contract for hardware that drives SessionManager state transitions.
 *
 * A `SessionControlCapable` component emits a small fixed set of
 * session-level intents (start episode, stop early, re-record, stop
 * session). The host installs callbacks via `set_callbacks()` and starts
 * the source with `start()`; the source then pushes events through the
 * callback — there is no polling. The source owns whatever threading it
 * needs to detect input (dedicated reader thread, piggyback on a hardware
 * I/O thread, etc.) and is responsible for keeping the callback cheap.
 *
 * Interpretation of events is state-dependent and lives in the host's
 * event handler: a `kStart` event means "begin the next episode" when
 * idle, "stop current and advance" while recording, and "skip reset"
 * during the between-episode wait. Sources do not need to know the
 * session's current phase.
 *
 * Disconnect is a one-shot signal. The host's disconnect handler typically
 * stops the current episode cleanly (saving the partial recording) and
 * ends the session.
 */

#ifndef TROSSEN_SDK__HW__SESSION_CONTROL__SESSION_CONTROL_CAPABLE_HPP_
#define TROSSEN_SDK__HW__SESSION_CONTROL__SESSION_CONTROL_CAPABLE_HPP_

#include <atomic>
#include <functional>
#include <utility>

namespace trossen::hw::session_control {

/**
 * @brief Intent emitted by a session-control source.
 *
 * Events are semantic intents, not raw inputs. `kStart` means "advance" —
 * its concrete effect depends on the current session phase and is
 * resolved by SessionManager, not the source.
 */
enum class SessionControlEvent {
  kNone,          ///< No event pending. Callbacks never pass this.
  kStart,         ///< Start / stop-early / skip-reset depending on phase.
  kStopEarly,     ///< Stop the current recording without advancing.
  kRerecord,      ///< Discard current (recording) or last (resetting).
  kStopSession,   ///< End the whole session (equivalent to Ctrl+C).
  kSummon         ///< Ease every follower onto its leader's current pose.
};

/**
 * @brief Push-based session-control input contract.
 *
 * A source detects user intent on its own thread and pushes it to the host
 * through the installed callbacks. Concrete sources implement `start()` /
 * `stop()` and, from their reader thread, call the protected `emit_event()`
 * and `signal_disconnect()` helpers rather than invoking the callbacks
 * directly — the base owns the callbacks and guarantees the disconnect
 * fires at most once per drop.
 *
 * @warning Callbacks run on the source's thread. The host must not call
 * single-threaded `SessionManager` episode methods (start/stop/discard)
 * directly from them; hand the intent to the main loop instead.
 */
class SessionControlCapable {
public:
  using EventCallback = std::function<void(SessionControlEvent)>;
  using DisconnectCallback = std::function<void()>;

  virtual ~SessionControlCapable() = default;

  /**
   * @brief Install the event and disconnect callbacks.
   *
   * Call before `start()`, and do not swap the callbacks while running — the
   * reader thread reads them without locking. A callback must not call the
   * owning component's own `stop()` (that would join the reader thread from
   * within itself — a deadlock); hand the intent to the main loop instead.
   */
  void set_callbacks(EventCallback on_event, DisconnectCallback on_disconnect) {
    event_cb_      = std::move(on_event);
    disconnect_cb_ = std::move(on_disconnect);
  }

  /**
   * @brief Begin producing events.
   *
   * Typically spawns the source's reader thread (keyboard) or hooks into
   * an existing one (VR frame callback). Idempotent: calling `start()`
   * on an already-started source is a no-op.
   */
  virtual void start() = 0;

  /**
   * @brief Stop producing events and join any internal threads.
   *
   * Must be safe to call from the source's destructor. Idempotent.
   * After `stop()` returns, no further callbacks will fire.
   */
  virtual void stop() = 0;

protected:
  /// Forward a user-intent event to the host. No-op if no callback is
  /// installed or the event is `kNone`.
  void emit_event(SessionControlEvent event) {
    if (event != SessionControlEvent::kNone && event_cb_) event_cb_(event);
  }

  /// Report that the source has dropped. Guaranteed to fire the host's
  /// disconnect callback at most once per drop: further calls are ignored
  /// until `arm_disconnect()` re-arms it. Every source gets fire-once
  /// behavior here instead of re-implementing it.
  void signal_disconnect() {
    if (disconnect_armed_.exchange(false) && disconnect_cb_) disconnect_cb_();
  }

  /// Re-arm disconnect detection; call when the source (re)connects so a
  /// later drop fires again.
  void arm_disconnect() { disconnect_armed_.store(true); }

private:
  EventCallback      event_cb_;
  DisconnectCallback disconnect_cb_;
  std::atomic<bool>  disconnect_armed_{true};
};

}  // namespace trossen::hw::session_control

#endif  // TROSSEN_SDK__HW__SESSION_CONTROL__SESSION_CONTROL_CAPABLE_HPP_
