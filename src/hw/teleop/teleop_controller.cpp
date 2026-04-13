/**
 * @file teleop_controller.cpp
 * @brief Implementation of TeleopController
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
}

TeleopController::~TeleopController() {
  stop();
}

void TeleopController::start() {
  if (running_.exchange(true)) {
    return;  // already running
  }

  // Prepare hardware: follower first (move to match), then leader (enable gravity comp)
  if (follower_) {
    auto initial_positions = leader_->get_joint_positions();
    follower_->prepare_for_follower(initial_positions);
    std::cout << "  [teleop] Follower prepared (matched leader positions)\n";
  }

  leader_->prepare_for_leader();
  std::cout << "  [teleop] Leader prepared (gravity compensation enabled)\n";

  thread_ = std::thread([this]() { control_loop(); });
}

void TeleopController::stop() {
  if (!running_.exchange(false)) {
    return;  // not running
  }

  if (thread_.joinable()) {
    thread_.join();
  }

  // Clean up hardware
  leader_->cleanup_teleop();
  if (follower_) {
    follower_->cleanup_teleop();
  }
}

void TeleopController::control_loop() {
  const auto period = std::chrono::nanoseconds(
    static_cast<int64_t>(1e9 / cfg_.control_rate_hz));

  std::vector<float> mapped_cmd;

  while (running_) {
    auto deadline = std::chrono::steady_clock::now() + period;

    // Read leader positions
    auto cmd = leader_->get_joint_positions();

    // Send to follower (if present)
    if (follower_) {
      if (!cfg_.joint_mapping.empty()) {
        // Apply joint remapping
        mapped_cmd.resize(cfg_.joint_mapping.size());
        for (size_t i = 0; i < cfg_.joint_mapping.size(); ++i) {
          int src_idx = cfg_.joint_mapping[i];
          mapped_cmd[i] = (src_idx >= 0 && src_idx < static_cast<int>(cmd.size()))
                              ? cmd[src_idx]
                              : 0.0f;
        }
        follower_->set_joint_positions(mapped_cmd);
      } else {
        follower_->set_joint_positions(cmd);
      }
    }

    // Sleep until next tick
    std::this_thread::sleep_until(deadline);
  }
}

}  // namespace trossen::hw::teleop
