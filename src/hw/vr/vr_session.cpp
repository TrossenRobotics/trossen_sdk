/**
 * @file vr_session.cpp
 * @brief Implementation of the process-global VR connection owner.
 */

#include "trossen_sdk/hw/vr/vr_session.hpp"

#include <stdexcept>
#include <string>
#include <thread>

namespace trossen::hw::vr {

namespace {

// Poll cadence for wait_for_connection. Chosen to feel interactive (50 ms is
// well under human perceptual thresholds) without burning CPU.
constexpr std::chrono::milliseconds kConnectPollInterval{50};

}  // namespace

VrSession& VrSession::instance() {
  static VrSession session;
  return session;
}

VrSession::~VrSession() {
  // The singleton outlives normal shutdown, but a test runner or a process
  // with explicit teardown may reach this path. Stopping the manager here
  // avoids leaking the I/O thread if ref counting was ever skipped.
  std::lock_guard<std::mutex> lock(mutex_);
  if (manager_) {
    manager_->stop();
    manager_.reset();
  }
}

void VrSession::ensure_started(std::uint16_t port) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (manager_) {
    if (port_ != port) {
      throw std::runtime_error(
        "VrSession: already started on port " + std::to_string(port_) +
        "; cannot also bind port " + std::to_string(port) +
        " in the same process");
    }
    ++ref_count_;
    return;
  }
  trossen_vr::VRManager::Config cfg;
  cfg.server_port = port;
  manager_ = std::make_unique<trossen_vr::VRManager>(cfg);
  manager_->start();
  port_      = port;
  ref_count_ = 1;
}

void VrSession::release() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ref_count_ == 0) return;
  if (--ref_count_ == 0 && manager_) {
    manager_->stop();
    manager_.reset();
    port_ = 0;
  }
}

bool VrSession::is_quest_connected() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return manager_ && manager_->is_connected();
}

std::optional<trossen_vr::VRState> VrSession::latest_frame() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!manager_) return std::nullopt;
  return manager_->get_current_state();
}

bool VrSession::wait_for_connection(
    std::chrono::milliseconds timeout) const {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (is_quest_connected()) return true;
    std::this_thread::sleep_for(kConnectPollInterval);
  }
  return is_quest_connected();
}

}  // namespace trossen::hw::vr
