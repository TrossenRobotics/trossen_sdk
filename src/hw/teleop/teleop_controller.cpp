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

  // Each arm stages itself using its own configured staging pose and
  // trajectory time. Arms that don't need staging override stage() with
  // a no-op. Calls here are expected to be non-blocking so multi-arm
  // setups stage in parallel.
  leader_->stage();
  if (follower_) {
    follower_->stage();
  }
  std::cout << "  [teleop] Staging initiated\n";
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

  // First-time driver bring-up: gated on `driver_prepared_`, not on
  // `running_`. `pause_teleop()` clears `running_` between episodes
  // without tearing down drivers, so we must not re-run the bring-up
  // on resume.
  if (!driver_prepared_) {
    if (follower_) {
      follower_->prepare_for_teleop();
    }
    leader_->prepare_for_teleop();
    driver_prepared_ = true;
    std::cout << "  [teleop] Arms ready for teleop\n";
  }

  // Always re-sync at the top of each episode so virtual leaders
  // (e.g. VR controllers) capture a fresh anchor against the
  // follower's current pose. For real-hardware leaders
  // sync_to_state is a no-op by default, so this is safe to always
  // run. The re-sync lets the operator reposition the VR controller
  // between episodes without the arm snapping on the next start.
  if (follower_io_) {
    leader_io_->sync_to_state(follower_io_->read());
  }
}

void TeleopController::teleop() {
  if (running_.exchange(true)) {
    return;
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
  // Stop the mirror thread but keep drivers prepared. This is the
  // between-episode pause — the arms stop following the leader so
  // the operator can reposition without the follower tracking, and
  // resume via `teleop()` skips the driver bring-up because
  // `driver_prepared_` is still true.
  running_.store(false);
  if (thread_.joinable()) {
    thread_.join();
  }
}

void TeleopController::restage() {
  // Mirror the constructor's staging call so the arms return to their
  // configured staging pose between episodes. sync_to_state captures
  // the new anchor on the next prepare_teleop() — callers should
  // invoke this after the current episode has stopped and before the
  // next one starts. Silent: the motion itself is the feedback.
  leader_->stage();
  if (follower_) {
    follower_->stage();
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
  // Clear the first-time gate so a subsequent prepare_teleop() (e.g.
  // during a full session restart) re-runs the driver bring-up.
  driver_prepared_ = false;
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
