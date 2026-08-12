/**
 * @file teleop_controller.cpp
 * @brief Implementation of TeleopController.
 */

#include "trossen_sdk/hw/teleop/teleop_controller.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace trossen::hw::teleop {

namespace {

/// steady_clock now, in nanoseconds. Steady rather than system time on
/// purpose: the watchdog measures an interval, and a clock step (NTP settling
/// after boot is routine on these rigs) must not read as a stalled link.
std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// RAII hold on the stall watchdog, for the mirror's deliberately blocking
/// sections.
///
/// Refreshes the tick clock on the way OUT as well, and before releasing the
/// hold: otherwise the loop resumes and is immediately judged against the
/// seconds it spent in a move the host asked for, which would fault every
/// summon rather than none.
class WatchdogPause {
public:
  WatchdogPause(std::atomic<int>& paused, std::atomic<std::int64_t>& last_read)
    : paused_(paused), last_read_(last_read) {
    paused_.fetch_add(1, std::memory_order_relaxed);
  }
  ~WatchdogPause() {
    last_read_.store(now_ns(), std::memory_order_relaxed);
    paused_.fetch_sub(1, std::memory_order_relaxed);
  }
  WatchdogPause(const WatchdogPause&) = delete;
  WatchdogPause& operator=(const WatchdogPause&) = delete;

private:
  std::atomic<int>& paused_;
  std::atomic<std::int64_t>& last_read_;
};

}  // namespace

TeleopController::TeleopController(
    std::shared_ptr<TeleopCapable> leader,
    std::shared_ptr<TeleopCapable> follower,
    Config config)
  : leader_(std::move(leader))
  , follower_(std::move(follower))
  , cfg_(std::move(config))
{
  if (!leader_) {
    throw std::invalid_argument("TeleopController: leader must not be null");
  }
  if (cfg_.control_rate_hz <= 0.0f || !std::isfinite(cfg_.control_rate_hz)) {
    throw std::invalid_argument(
      "TeleopController: control_rate_hz must be positive and finite");
  }
  if (cfg_.leader_timeout_ms < 0.0f || !std::isfinite(cfg_.leader_timeout_ms)) {
    throw std::invalid_argument(
      "TeleopController: leader_timeout_ms must be non-negative and finite "
      "(0 disables the watchdog)");
  }

  // Resolve the requested space on both sides. Throws if the hardware does
  // not implement the required space child class.
  resolve_space_views();

  // Staging to a home pose is not the controller's concern. The SessionManager
  // drives it per episode by calling HardwareComponent::on_pre_episode() on each
  // opted-in component (e.g. an arm moving to its staged pose) while the mirror
  // loop is paused, so it happens from a known state before teleop restarts.
}

TeleopController::~TeleopController() {
  // Signal stop (may already be false if the loop exited via exception)
  // and always join if the thread is still joinable. Skipping join on a
  // joinable thread causes std::terminate at destruction.
  running_.store(false);
  // Before the mirror: the watchdog can still fire a fault, and a callback
  // reaching into a half-destroyed controller is worse than a late one.
  join_watchdog();
  if (thread_.joinable()) {
    thread_.join();
  }
}

// ── Space resolution ─────────────────────────────────────────────────────

void TeleopController::resolve_space_views() {
  auto resolve = [this](const std::shared_ptr<TeleopCapable>& hw,
                        const char* role) -> TeleopTypeIO* {
    TeleopTypeIO* io = hw->as_space_io(cfg_.space);
    if (!io) {
      throw std::invalid_argument(
        std::string("TeleopController: ") + role +
        " does not implement " + std::string(space_iface_name(cfg_.space)) +
        " (" + std::string(space_name(cfg_.space)) +
        "-space teleop is not available for this hardware)");
    }
    return io;
  };

  leader_io_ = resolve(leader_, "leader");
  if (follower_) {
    follower_io_ = resolve(follower_, "follower");
  }

  std::cout << "  [teleop] Space: " << space_name(cfg_.space) << "\n";
}

// ── Lifecycle ────────────────────────────────────────────────────────────

void TeleopController::prepare_teleop() {
  leader_->pre_episode();
  if (follower_) {
    follower_->pre_episode();
  }

  // The mirror loop runs continuously across episodes; return early if it
  // is already running.
  if (running_) {
    return;
  }

  // First-episode setup. Each arm reads its configured role and trajectory
  // parameters from its own members; the controller only signals the
  // lifecycle transition.
  if (follower_) {
    follower_->prepare_for_teleop();
  }
  leader_->prepare_for_teleop();

  // Let virtual leaders align their internal state to the follower's
  // current pose before the mirror loop starts. Real-hardware leaders
  // inherit the no-op default.
  if (follower_io_) {
    leader_io_->sync_to_state(follower_io_->read());
  }

  // Summon the follower onto the leader's current pose with a smooth, blocking
  // move so it eases into position instead of snapping there on the first
  // mirror tick. Essential for a passive leader, which can be anywhere at the
  // start of teleop. summon() falls back to an instant write on hardware that
  // doesn't implement a timed move, so this is safe for every follower.
  if (follower_io_) {
    const auto leader_pose = leader_io_->read();
    if (!leader_pose.empty()) {
      std::cout << "  [teleop] Summoning follower to leader pose...\n";
      follower_io_->summon(leader_pose);
    }
  }
  std::cout << "  [teleop] Arms ready for teleop\n";
}

void TeleopController::teleop() {
  if (running_.exchange(true)) {
    return;
  }
  // Reap a previous loop that exited on its own: control_loop() catches and
  // sets running_=false on exception but does not join, so the finished thread
  // stays joinable. Assigning to a joinable std::thread calls std::terminate,
  // so join any stale thread before starting a new one.
  if (thread_.joinable()) {
    thread_.join();
  }
  // Reap a watchdog left over from a previous run before re-arming.
  join_watchdog();

  // Re-arm fault reporting for this run, and seed the tick clock so the
  // watchdog measures from the start of teleop rather than from an epoch that
  // would look like an instant stall.
  fault_pending_.store(true);
  last_read_ns_.store(now_ns());

  thread_ = std::thread([this]() { control_loop(); });

  if (cfg_.leader_timeout_ms > 0.0f) {
    watchdog_active_.store(true);
    watchdog_thread_ = std::thread([this]() { watchdog_loop(); });
  }
}

void TeleopController::reset_teleop() {
  leader_->post_episode();
  if (follower_) {
    follower_->post_episode();
  }
}

void TeleopController::pause_teleop() {
  // Stop the mirror thread but keep the drivers alive. running_ is left false
  // so the next prepare_teleop() re-arms teleop modes and teleop() can restart
  // the loop. Unlike stop_teleop(), end_teleop() is not called.
  running_.store(false);
  // A deliberate pause is not a stall. Stopping the watchdog first means a
  // between-episode re-stage cannot be reported as a dead link.
  join_watchdog();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void TeleopController::stop_teleop() {
  running_.store(false);
  join_watchdog();
  if (thread_.joinable()) {
    thread_.join();
  }
  // A repeated stop_teleop() is harmless: the arms' end_teleop()
  // implementations are expected to be idempotent once their driver has
  // been released.
  leader_->end_teleop();
  if (follower_) {
    follower_->end_teleop();
  }
}

bool TeleopController::request_summon() {
  // Dropped rather than queued when the mirror is not running: the point of a
  // summon is "go to where the leader is NOW", and a request serviced at some
  // later restart would drive to a pose the operator has long since left.
  if (!follower_io_ || !running_.load()) {
    std::cerr << "  [teleop] summon ignored — "
              << (follower_io_ ? "mirror loop is not running" : "no follower configured")
              << '\n';
    return false;
  }
  summon_requested_.store(true);
  return true;
}

// ── Control loop ────────────────────────────────────────────────────────

void TeleopController::control_loop() {
  const auto period = std::chrono::nanoseconds(
    static_cast<int64_t>(1e9 / cfg_.control_rate_hz));

  // Which side we are talking to right now, so the catch below can name the
  // culprit instead of reporting "something threw". Cheap to maintain — a
  // plain local, written a few times per tick on the thread that reads it.
  FaultCause phase = FaultCause::kUnknown;

  // An uncaught exception would invoke std::terminate (thread functions are
  // implicitly noexcept at the boundary). Catch and report so that
  // prepare_teleop() can observe the mirror as stopped AND the host learns
  // why — before this had a fault callback, a dead leader produced one stderr
  // line and a session that carried on recording a follower nobody was driving.
  try {
    while (running_) {
      auto deadline = std::chrono::steady_clock::now() + period;

      // Serviced here, on the loop thread, so the blocking move cannot race
      // the high-rate writes below. The deadline for this tick is abandoned:
      // summon() takes seconds by design, so there is nothing to catch up to
      // and the next tick simply starts fresh.
      if (summon_requested_.exchange(false)) {
        if (follower_io_) {
          phase = FaultCause::kLeaderError;
          const auto pose = leader_io_->read();
          last_read_ns_.store(now_ns(), std::memory_order_relaxed);
          if (!pose.empty()) {
            std::cout << "  [teleop] Summoning follower to leader pose...\n";
            phase = FaultCause::kFollowerError;
            {
              // Hold the watchdog off for the duration: this move is seconds
              // long and reads nothing, so it is indistinguishable from a dead
              // link by age alone.
              WatchdogPause pause(watchdog_paused_, last_read_ns_);
              follower_io_->summon(pose);
            }
            std::cout << "  [teleop] Summon complete\n";
            // Published only here, after the move returns, so a caller waiting
            // on this count never sees "arrived" while the arm is still moving.
            // An empty leader read deliberately does not count: nothing moved,
            // so reporting a completion would let a waiter start recording from
            // a pose the follower never reached.
            summons_completed_.fetch_add(1, std::memory_order_release);
          }
        }
        continue;
      }

      phase = FaultCause::kLeaderError;
      auto cmd = leader_io_->read();
      // The heartbeat the watchdog measures against. Stored after the read
      // RETURNS, so a call that blocks forever never refreshes it — which is
      // the whole point on a link that blackholes rather than resets.
      last_read_ns_.store(now_ns(), std::memory_order_relaxed);

      if (follower_io_) {
        phase = FaultCause::kFollowerError;
        follower_io_->write(cmd);
      }

      // Reverse channel: reflect the follower's measured gripper effort back
      // onto the leader so the operator feels the grasp. Only runs when the
      // leader renders feedback (e.g. a passive-arm leader with an actuated
      // gripper); otherwise both calls are skipped.
      if (follower_io_ && leader_io_->renders_gripper_feedback()) {
        if (const auto effort = follower_io_->read_gripper_effort()) {
          leader_io_->apply_gripper_feedback(*effort);
        }

        std::vector<float> efforts = follower_io_->read_multiple_gripper_efforts();
        if (efforts.size() >= 1) {
          leader_io_->apply_multiple_gripper_feedback(efforts);
        }
      }

      std::this_thread::sleep_until(deadline);
    }
  } catch (const std::exception& e) {
    report_fault(phase, e.what());
  } catch (...) {
    report_fault(FaultCause::kUnknown, "non-std::exception thrown by the mirror loop");
  }
}

// ── Fault reporting and the stall watchdog ──────────────────────────────

void TeleopController::report_fault(FaultCause cause, std::string detail) {
  // Stop first, report second. Whatever the host does with the fault, the
  // mirror must already have given up on the hardware by the time it hears —
  // a callback that stages a recovery while the loop is still writing to a
  // half-dead follower is the race this ordering removes.
  running_.store(false);
  watchdog_active_.store(false);

  // One fault per run. Leader and follower usually fail together (a dropped
  // link takes both), and the host only needs the first cause to decide what
  // to do; re-arming happens in teleop().
  if (!fault_pending_.exchange(false)) {
    return;
  }

  // Kept on stderr as well as the callback: this line predates the callback
  // and is what shows up in the recorder's log, which is the only record when
  // no host callback is installed (tests, examples, leader-only rigs).
  std::cerr << "  [teleop] mirror loop terminated: " << detail << '\n';

  if (fault_cb_) {
    fault_cb_(Fault{cause, std::move(detail)});
  }
}

void TeleopController::watchdog_loop() {
  const auto timeout_ns =
    static_cast<std::int64_t>(cfg_.leader_timeout_ms * 1e6);

  // Poll at a quarter of the deadline so worst-case detection is ~1.25x the
  // configured timeout rather than 2x, with a 20 ms floor so a very short
  // deadline cannot turn this into a spin loop on a busy Orin.
  const auto tick = std::max<std::int64_t>(
    20'000'000, timeout_ns / 4);

  while (watchdog_active_.load()) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(tick));

    // Re-checked after the sleep: stop_teleop() may have run while we slept,
    // and faulting a mirror that was deliberately stopped would end a session
    // the operator just ended themselves.
    if (!watchdog_active_.load()) {
      return;
    }
    if (watchdog_paused_.load(std::memory_order_relaxed) > 0) {
      continue;
    }

    const auto age_ns = now_ns() - last_read_ns_.load(std::memory_order_relaxed);
    if (age_ns > timeout_ns) {
      report_fault(
        FaultCause::kLeaderStalled,
        "no leader read for " + std::to_string(age_ns / 1'000'000) +
        " ms (limit " + std::to_string(static_cast<std::int64_t>(cfg_.leader_timeout_ms)) +
        " ms) — the leader link is not delivering");
      return;
    }
  }
}

void TeleopController::join_watchdog() {
  watchdog_active_.store(false);
  if (watchdog_thread_.joinable()) {
    watchdog_thread_.join();
  }
}

}  // namespace trossen::hw::teleop
