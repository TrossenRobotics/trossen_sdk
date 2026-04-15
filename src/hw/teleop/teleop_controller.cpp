/**
 * @file teleop_controller.cpp
 * @brief Implementation of TeleopController.
 */

#include "trossen_sdk/hw/teleop/teleop_controller.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

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
  // The destructor only needs to stop the mirror thread. Each hardware
  // component is responsible for releasing its own driver in its own
  // destructor — this avoids calling into potentially-torn-down hardware
  // from a destructor that must not throw.
  if (running_.exchange(false) && thread_.joinable()) {
    thread_.join();
  }
}

// ── Space resolution ─────────────────────────────────────────────────────

void TeleopController::resolve_space_views() {
  auto resolve = [this](const std::shared_ptr<TeleopCapable>& hw,
                        const char* role) -> TeleopSpaceIO* {
    TeleopSpaceIO* io = hw->as_space_io(cfg_.space);
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

  // The mirror loop runs continuously across episodes, keeping the follower
  // at the leader's current state through reset periods. Once it is
  // running, per-episode preparation is unnecessary.
  if (running_) {
    return;
  }

  // First-episode setup. Each arm reads its configured role (leader vs
  // follower) and trajectory parameters from its own members; the
  // controller only signals the lifecycle transition.
  if (follower_) {
    follower_->prepare_for_teleop();
  }
  leader_->prepare_for_teleop();

  // Give virtual leaders (e.g. keyboard input) a chance to align their
  // internal state to the follower's actual pose, so the first mirror
  // tick doesn't snap. Real-hardware leaders inherit the no-op default.
  if (follower_io_) {
    leader_io_->sync_to_state(follower_io_->read());
  }
  std::cout << "  [teleop] Arms ready for teleop\n";
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

void TeleopController::stop_teleop() {
  if (running_.exchange(false) && thread_.joinable()) {
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
