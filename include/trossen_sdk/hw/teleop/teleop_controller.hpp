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
#include <memory>
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
  };

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
   * follower at control_rate_hz. No-op if already running. Also
   * resumes from a prior `pause_teleop()` — drivers stay prepared
   * across a pause/resume cycle, so a fresh driver bring-up is not
   * needed on resume.
   */
  void teleop();

  /**
   * @brief Pause the mirror loop without tearing down drivers.
   *
   * Joins the control thread but does not call `end_teleop()`. The
   * leader and follower keep their prepared driver state, so a
   * subsequent `teleop()` resumes mirroring without repeating the
   * first-time bring-up. Intended for the reset window between
   * episodes, where the arms should stop following the leader so
   * the operator can reposition safely.
   */
  void pause_teleop();

  /**
   * @brief Enter reset mode.
   *
   * Calls post_episode() on both components. The mirror loop keeps running,
   * so the user can move the leader freely and the follower continues to
   * track. Recording is handled separately by the session manager.
   */
  void reset_teleop();

  /**
   * @brief Stop the mirror loop and return hardware to rest.
   *
   * Joins the control thread, then calls end_teleop() on both components
   * to neutralize and release driver resources.
   */
  void stop_teleop();

  /**
   * @brief Re-run the staging poses on both leader and follower.
   *
   * The constructor stages once at session start. Callers that want
   * the arms returned to their staging pose between episodes invoke
   * this explicitly — typically from a SessionManager
   * `on_episode_ended` callback so the operator can pause, reposition
   * the leader (e.g. a VR controller), and start the next episode
   * from a known-safe configuration. The follower sync_to_state on
   * the next `prepare_teleop()` call captures the new VR anchor.
   *
   * No-op for hardware that overrides `stage()` with a no-op default.
   */
  void restage();

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

  std::shared_ptr<TeleopCapable> leader_;
  std::shared_ptr<TeleopCapable> follower_;

  // Space-specific IO views resolved from the leader/follower components.
  // Non-owning — lifetime is tied to the shared_ptrs above.
  TeleopTypeIO* leader_io_{nullptr};
  TeleopTypeIO* follower_io_{nullptr};

  Config cfg_;
  std::thread thread_;
  std::atomic<bool> running_{false};

  /// @brief First-time driver bring-up has completed.
  ///
  /// Distinct from `running_`: `running_` tracks whether the mirror
  /// thread is live, while `driver_prepared_` tracks whether
  /// `prepare_for_teleop()` has been dispatched to the leader and
  /// follower. `pause_teleop()` clears `running_` but leaves the
  /// drivers prepared, so a subsequent `teleop()` resumes without a
  /// second driver bring-up.
  bool driver_prepared_{false};
};

}  // namespace trossen::hw::teleop

#endif  // TROSSEN_SDK__HW__TELEOP__TELEOP_CONTROLLER_HPP
