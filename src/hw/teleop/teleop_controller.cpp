/**
 * @file teleop_controller.cpp
 * @brief Implementation of TeleopController.
 */

#include "trossen_sdk/hw/teleop/teleop_controller.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace trossen::hw::teleop {

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
  thread_ = std::thread([this]() { control_loop(); });
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
  if (thread_.joinable()) {
    thread_.join();
  }
}

void TeleopController::stop_teleop() {
  running_.store(false);
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

// ── Control loop ────────────────────────────────────────────────────────

void TeleopController::control_loop() {
  const auto period = std::chrono::nanoseconds(
    static_cast<int64_t>(1e9 / cfg_.control_rate_hz));

  // An uncaught exception would invoke std::terminate (thread functions are
  // implicitly noexcept at the boundary). Catch and log so that
  // prepare_teleop() can observe the mirror as stopped.
  try {
    while (running_) {
      auto deadline = std::chrono::steady_clock::now() + period;

      auto cmd = leader_io_->read();
      if (follower_io_) {
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
    std::cerr << "  [teleop] mirror loop terminated: " << e.what() << '\n';
    running_.store(false);
  } catch (...) {
    std::cerr << "  [teleop] mirror loop terminated with unknown error\n";
    running_.store(false);
  }
}

}  // namespace trossen::hw::teleop
