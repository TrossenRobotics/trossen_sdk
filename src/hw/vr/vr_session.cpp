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

bool is_valid_controller_type(const std::string& controller_type) {
  return controller_type == "left" || controller_type == "right";
}

}  // namespace

std::string_view vr_input_name(VrInput input) {
  switch (input) {
    case VrInput::kPose:       return "pose";
    case VrInput::kTrigger:    return "trigger";
    case VrInput::kThumbstick: return "thumbstick";
    case VrInput::kButtonOne:    return "button_one";
    case VrInput::kButtonTwo:    return "button_two";
  }
  return "unknown";
}

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
  trossen_vr::ReceiverConfig cfg;
  cfg.port = port;
  manager_ = std::make_unique<trossen_vr::NetworkManager>(cfg);
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

bool VrSession::is_vr_connected() const {
  // "Connected" here means "receiving frames," not just "socket open."
  std::lock_guard<std::mutex> lock(mutex_);
  if (!manager_) return false;
  auto status = manager_->get_connection_status();
  return status == trossen_vr::ConnectionStatus::Connected ||
         status == trossen_vr::ConnectionStatus::Degraded;
}

std::optional<trossen_vr::VRFrame> VrSession::latest_frame() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!manager_) return std::nullopt;
  return manager_->latest_frame();
}

bool VrSession::wait_for_connection(
    std::chrono::milliseconds timeout) const {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (is_vr_connected()) return true;
    std::this_thread::sleep_for(kConnectPollInterval);
  }
  return is_vr_connected();
}

void VrSession::claim_inputs(const std::string& controller_type,
                             const std::string& component_id,
                             std::initializer_list<VrInput> inputs) {
  if (!is_valid_controller_type(controller_type)) {
    throw std::invalid_argument(
      "VrSession::claim_inputs: controller_type must be 'left' or 'right', got '" +
      controller_type + "'");
  }
  if (component_id.empty()) {
    throw std::invalid_argument(
      "VrSession::claim_inputs: component_id must not be empty");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  // First pass: conflict detection. Done before any mutation so a
  // partial claim never leaks on failure.
  for (const auto input : inputs) {
    const ClaimKey key{controller_type, input};
    auto it = claims_.find(key);
    if (it != claims_.end() && it->second != component_id) {
      throw std::runtime_error(
        std::string{"VrSession::claim_inputs: '"} +
        std::string{vr_input_name(input)} + "' on controller type '" + controller_type +
        "' is already claimed by '" + it->second +
        "'; requested by '" + component_id + "'");
    }
  }
  // Second pass: apply. Idempotent when the same component re-claims.
  for (const auto input : inputs) {
    claims_[ClaimKey{controller_type, input}] = component_id;
  }
}

void VrSession::release_claims(const std::string& component_id) {
  if (component_id.empty()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = claims_.begin(); it != claims_.end(); ) {
    if (it->second == component_id) {
      it = claims_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace trossen::hw::vr
