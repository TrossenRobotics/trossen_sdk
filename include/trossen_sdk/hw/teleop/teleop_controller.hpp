/**
 * @file teleop_controller.hpp
 * @brief Teleop controller that mirrors a leader's state to a follower.
 *
 * The controller takes two TeleopCapable instances (leader and optional
 * follower) plus a teleop space chosen in configuration. It resolves the
 * space-specific IO view from each component via `as_space_io(Space)` and
 * runs a high-rate loop that reads the leader and writes to the follower in
 * that space. If either side does not implement the requested space,
 * construction throws with a clear message.
 *
 * The follower may be nullptr for leader-only setups. Recording is
 * performed separately and is not the controller's concern.
 */

#ifndef TROSSEN_SDK__HW__TELEOP__TELEOP_CONTROLLER_HPP
#define TROSSEN_SDK__HW__TELEOP__TELEOP_CONTROLLER_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "trossen_sdk/hw/teleop/teleop_capable.hpp"

namespace trossen::hw::teleop {

class TeleopController {
public:
  struct Config {
    /// Teleop space. Both leader and follower must implement it.
    TeleopCapable::Space space{TeleopCapable::Space::Joint};

    /// Control loop rate in Hz (how fast leader state is mirrored to follower).
    float control_rate_hz{1000.0f};

    /// How long the mirror may go without a successful leader read before the
    /// link is declared dead, in milliseconds. 0 disables the watchdog.
    ///
    /// Not merely belt-and-braces over the exception path: a leader reached
    /// through a proxy-ARP bridge (the Rivet's Glide handles arrive over
    /// `parprouted` on the cockpit Pi) fails by BLACKHOLING frames, not by
    /// refusing them. The read blocks instead of throwing, so without a
    /// deadline the mirror waits forever and the session records a follower
    /// that stopped tracking minutes ago.
    ///
    /// Off by default because it is only meaningful where the leader is
    /// remote: on a wired rig a stall means the arm itself is wedged, which
    /// the exception path already reports. Opt in per rig.
    float leader_timeout_ms{0.0f};
  };

  /// Why the mirror stopped without anyone asking it to.
  ///
  /// The distinction that matters downstream is *stalled* versus *threw*: a
  /// stall is a dead link and the arm may be perfectly healthy, while a throw
  /// usually means the controller latched an error and idled every joint.
  /// They need different recovery, so they are not collapsed into one code.
  enum class FaultCause {
    kLeaderStalled,   ///< No successful leader read inside `leader_timeout_ms`.
    kLeaderError,     ///< Reading the leader threw.
    kFollowerError,   ///< Writing the follower threw.
    kUnknown          ///< Non-`std::exception` throw; nothing to report but the fact.
  };

  struct Fault {
    FaultCause cause{FaultCause::kUnknown};
    /// Exception text, or for a stall how long the mirror went unserviced.
    std::string detail;
  };

  using FaultCallback = std::function<void(const Fault&)>;

  /**
   * @brief Construct a teleop controller.
   *
   * @param leader   Teleop-capable component to read state from.
   * @param follower Teleop-capable component to write state to (nullptr for
   *                 leader-only).
   * @param config   Controller configuration, including the teleop space.
   *
   * @throws std::invalid_argument if `leader` is null.
   * @throws std::invalid_argument if the leader (or non-null follower) does
   *         not implement the requested space — e.g. requesting cartesian
   *         on hardware that only inherits JointSpaceTeleop.
   */
  TeleopController(
    std::shared_ptr<TeleopCapable> leader,
    std::shared_ptr<TeleopCapable> follower,
    Config config);

  ~TeleopController();

  // Non-copyable, non-movable (owns a thread).
  TeleopController(const TeleopController&) = delete;
  TeleopController& operator=(const TeleopController&) = delete;

  /**
   * @brief Prepare hardware for a teleop episode.
   *
   * Always dispatches pre_episode() on both components. If the mirror loop
   * is already running, returns after that — the follower is tracking the
   * leader continuously and no further setup is needed. On the first call
   * (before the mirror starts), also prepares teleop modes on both
   * components and calls sync_to_state so virtual leaders can align with
   * the follower. Does not start the mirror thread — that is teleop().
   */
  void prepare_teleop();

  /**
   * @brief Start the mirror loop.
   *
   * Spawns the control thread that reads the leader and writes to the
   * follower at control_rate_hz. No-op if already running; the mirror runs
   * continuously across episodes.
   */
  void teleop();

  /**
   * @brief Enter reset mode.
   *
   * Calls post_episode() on both components. The mirror loop keeps running,
   * so the user can move the leader freely and the follower continues to
   * track. Recording is handled separately by the session manager.
   */
  void reset_teleop();

  /**
   * @brief Pause the mirror loop without releasing the hardware.
   *
   * Joins the control thread but, unlike stop_teleop(), does NOT call
   * end_teleop() — the drivers stay configured and the robots hold their last
   * commanded pose. After this, the robots can be re-staged, and a subsequent
   * prepare_teleop() re-arms teleop modes (running_ is false again) followed
   * by teleop() to restart the loop. Idempotent; no-op if not running.
   *
   * Used to re-stage robots to their home pose between episodes without tearing
   * down teleop.
   */
  void pause_teleop();

  /**
   * @brief Stop the mirror loop and return hardware to rest.
   *
   * Joins the control thread, then calls end_teleop() on both components
   * to neutralize and release driver resources.
   */
  void stop_teleop();

  /**
   * @brief Ask the follower to ease onto the leader's current pose.
   *
   * Thread-safe and non-blocking: it raises a flag that the MIRROR LOOP
   * services at the top of its next tick, on the loop's own thread. That is
   * the whole point — summon() is a blocking, time-parameterised move, and
   * running it on the loop thread means the mirror cannot be writing
   * high-rate commands to the same follower while it is in flight. Pausing
   * and restarting the thread would work too, but it would re-arm teleop
   * modes and re-run the start-of-teleop summon as a side effect.
   *
   * A no-op when there is no follower or the loop is not running — the pose
   * would be stale by the time the loop restarted. Repeated calls before the
   * loop services one collapse into a single summon.
   *
   * @return True if the request was accepted and the loop will service it;
   *   false if it was dropped (no follower, or the mirror is stopped). A caller
   *   that must not proceed until the follower has actually arrived needs both
   *   this and `summons_completed()` — the return only says the request was
   *   taken, not that the move has finished.
   */
  bool request_summon();

  /**
   * @brief Count of summons the mirror loop has carried through to completion.
   *
   * Monotonic, and incremented only after `summon()` returns — so it never
   * reports a move that is still in flight, and never one that was skipped
   * because the leader read came back empty.
   *
   * The counter exists because no other state can express "the follower has
   * arrived". `summon_requested_` is cleared BEFORE the blocking move starts
   * (the loop exchanges it to claim the request), so it reads false throughout
   * the seconds the arm is actually moving. Sample this, call
   * `request_summon()`, then wait for the value to change; comparing counts
   * rather than watching a flag also makes the wait immune to a second summon
   * landing in between.
   */
  std::uint64_t summons_completed() const {
    return summons_completed_.load(std::memory_order_acquire);
  }

  /**
   * @brief Install the callback fired when the mirror stops on its own.
   *
   * Call before `teleop()`; the callback is read without locking from both
   * the mirror thread and the watchdog thread. Fires AT MOST ONCE per run and
   * is re-armed by the next `teleop()`, so a fault that takes down leader and
   * follower together reports one cause rather than a burst.
   *
   * @warning The callback runs on whichever thread detected the fault, never
   * the caller's. It must not call `stop_teleop()` or `pause_teleop()` — both
   * join the very thread the callback is running on, which deadlocks. Hand the
   * intent to the host's main loop instead, exactly as SessionControlCapable
   * requires of its disconnect callback.
   */
  void set_fault_callback(FaultCallback cb) { fault_cb_ = std::move(cb); }

  /// @brief Check if the control loop is running.
  bool is_running() const { return running_.load(); }

  /// @brief Access the leader component (for session lifecycle calls).
  std::shared_ptr<TeleopCapable> leader() const { return leader_; }

  /// @brief Access the follower component (may be nullptr for leader-only).
  std::shared_ptr<TeleopCapable> follower() const { return follower_; }

  /// @brief The teleop space this controller was configured for.
  TeleopCapable::Space space() const { return cfg_.space; }

private:
  void resolve_space_views();
  void control_loop();

  /// Watchdog thread body. Runs only when `leader_timeout_ms > 0`.
  void watchdog_loop();

  /// Fire the fault callback, at most once between `teleop()` calls, and stop
  /// the mirror. Safe to call from either thread.
  void report_fault(FaultCause cause, std::string detail);

  /// Join the watchdog if it is running. Separate from the mirror join because
  /// every path that stops the mirror must also stop the watchdog, or it goes
  /// on declaring a stall against a loop that was deliberately paused.
  void join_watchdog();

  std::shared_ptr<TeleopCapable> leader_;
  std::shared_ptr<TeleopCapable> follower_;

  // Space-specific IO views resolved from the leader/follower components.
  // Non-owning — lifetime is tied to the shared_ptrs above.
  TeleopTypeIO* leader_io_{nullptr};
  TeleopTypeIO* follower_io_{nullptr};

  Config cfg_;
  std::thread thread_;
  std::atomic<bool> running_{false};

  /// Set by request_summon(), cleared by the mirror loop when it performs the
  /// move. Atomic because it crosses from a session-control thread onto the
  /// loop thread.
  std::atomic<bool> summon_requested_{false};

  /// Bumped by the mirror loop after each completed summon; see
  /// summons_completed(). Separate from the flag above because the flag is
  /// consumed before the move begins.
  std::atomic<std::uint64_t> summons_completed_{0};

  FaultCallback fault_cb_;

  /// Watchdog thread, spawned by teleop() only when the timeout is configured.
  std::thread watchdog_thread_;

  /// steady_clock nanoseconds at the last successful leader read. Written by
  /// the mirror thread, read by the watchdog — the one piece of state that
  /// tells a blocked read apart from a slow one.
  std::atomic<std::int64_t> last_read_ns_{0};

  /// Claimed by whichever thread reports first, so leader and follower failing
  /// together produce one fault. Reset by teleop().
  std::atomic<bool> fault_pending_{true};

  /// Cleared to stop the watchdog independently of `running_`: the mirror sets
  /// running_ = false as it unwinds, and the watchdog must not read that as a
  /// reason to keep waiting for a tick that will never come.
  std::atomic<bool> watchdog_active_{false};

  /// Non-zero while the mirror is inside a deliberately blocking call, so the
  /// watchdog holds off. A summon is a timed move measured in SECONDS — far
  /// longer than any sane leader deadline — and it does not read the leader
  /// while it runs. Without this the first summon of every episode would trip
  /// the watchdog and kill the session it was meant to protect.
  std::atomic<int> watchdog_paused_{0};
};

}  // namespace trossen::hw::teleop

#endif  // TROSSEN_SDK__HW__TELEOP__TELEOP_CONTROLLER_HPP
