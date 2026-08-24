/**
 * @file session_control_capable.hpp
 * @brief Mixin contract for hardware that drives SessionManager state transitions.
 *
 * A `SessionControlCapable` component emits a small fixed set of
 * session-level intents (start episode, stop early, re-record, stop
 * session). SessionManager attaches exactly one source at a time via
 * `attach_session_control()` and listens for events through a callback —
 * there is no polling. The source owns whatever threading it needs to
 * detect input (dedicated reader thread, piggyback on a hardware I/O
 * thread, etc.) and is responsible for keeping the callback cheap.
 *
 * Interpretation of events is state-dependent and lives entirely in
 * SessionManager: a `kStart` event means "begin the next episode" in
 * IDLE, "stop current and advance" in RECORDING, and "skip reset" in
 * RESETTING. Sources do not need to know the session's current phase.
 *
 * Disconnect is a one-shot signal. On disconnect the SessionManager
 * stops the current episode cleanly (saving the partial recording)
 * and ends the session — there is no fallback to another source.
 */

#ifndef TROSSEN_SDK__HW__SESSION_CONTROL__SESSION_CONTROL_CAPABLE_HPP_
#define TROSSEN_SDK__HW__SESSION_CONTROL__SESSION_CONTROL_CAPABLE_HPP_

#include <functional>

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
  kStopSession    ///< End the whole session (equivalent to Ctrl+C).
};

/**
 * @brief Push-based session-control input contract.
 *
 * Implementations call `on_event` once per detected user intent and
 * `on_disconnect` once if the source becomes permanently unusable.
 * Both callbacks fire from the source's own thread; implementations
 * must make them cheap and non-blocking.
 */
class SessionControlCapable {
public:
  using EventCallback      = std::function<void(SessionControlEvent)>;
  using DisconnectCallback = std::function<void()>;

  virtual ~SessionControlCapable() = default;

  /**
   * @brief Install the event and disconnect callbacks.
   *
   * Called by `SessionManager::attach_session_control()`. Implementations
   * store the callbacks for use by their reader thread(s). A subsequent
   * call replaces the previous callbacks.
   *
   * @note Callbacks may be invoked from any thread the source owns.
   */
  virtual void set_callbacks(EventCallback on_event,
                             DisconnectCallback on_disconnect) = 0;

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
};

}  // namespace trossen::hw::session_control

#endif  // TROSSEN_SDK__HW__SESSION_CONTROL__SESSION_CONTROL_CAPABLE_HPP_
