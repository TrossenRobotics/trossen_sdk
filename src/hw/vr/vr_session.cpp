/**
 * @file vr_session.cpp
 * @brief Implementation of the process-global VR connection owner.
 */

#include "trossen_sdk/hw/vr/vr_session.hpp"

#include <stdexcept>
#include <string>
#include <thread>
#include <variant>

namespace trossen::hw::vr {

namespace {

// Poll cadence for wait_for_connection. Chosen to feel interactive (50 ms is
// well under human perceptual thresholds) without burning CPU.
constexpr std::chrono::milliseconds kConnectPollInterval{50};

bool is_valid_hand(const std::string& hand) {
  return hand == "left" || hand == "right";
}

}  // namespace

std::string_view vr_input_name(VrInput input) {
  switch (input) {
    case VrInput::kPose:       return "pose";
    case VrInput::kTrigger:    return "trigger";
    case VrInput::kGrip:       return "grip";
    case VrInput::kThumbstick: return "thumbstick";
    case VrInput::kButtonA:    return "button_a";
    case VrInput::kButtonB:    return "button_b";
    case VrInput::kMenu:       return "menu";
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
  // "Connected" here means "receiving frames," not just "WebSocket open." The
  // trossen_vr library's own `is_connected()` flag is not reliably toggled on
  // when the client connects, and — more importantly — a WebSocket that is
  // open but idle is useless for teleop. Presence of a recent `VRState` is
  // the signal the caller actually cares about.
  std::lock_guard<std::mutex> lock(mutex_);
  if (!manager_) return false;
  return manager_->get_current_state().has_value();
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

bool VrSession::consume_start_signal(const std::string& controller_hand) {
  // Hold the session mutex for the whole call: we touch both the VR manager
  // (via get_current_state()) and the rising-edge state tracked on this
  // object. Both are cheap, so taking one lock keeps reasoning simple.
  std::lock_guard<std::mutex> lock(mutex_);
  if (!manager_) return false;

  const auto frame_opt = manager_->get_current_state();
  if (!frame_opt) return false;
  const auto& frame = *frame_opt;

  // (1) VRCommand::Start fires once per frame sequence. Using the frame's
  // sequence number (monotonically increasing) guards against re-firing on
  // the same frame if the caller polls faster than new frames arrive.
  if (frame.command.has_value() &&
      *frame.command == trossen_vr::VRCommand::Start &&
      frame.sequence > last_start_sequence_) {
    last_start_sequence_ = frame.sequence;
    return true;
  }

  // (2) A-button rising edge on the configured hand. The VR protocol names
  // digital buttons with the hand as a suffix-or-bare key; the shipping
  // Meta Quest app uses bare "a"/"b" today, so accept both shapes.
  const std::string suffixed_key = controller_hand + "_a";
  const auto* press_ptr = [&]() -> const bool* {
    auto it = frame.buttons.find(suffixed_key);
    if (it == frame.buttons.end()) it = frame.buttons.find("a");
    if (it == frame.buttons.end()) return nullptr;
    return std::get_if<bool>(&it->second);
  }();

  bool& prev = prev_a_button_[controller_hand];
  const bool current = press_ptr ? *press_ptr : false;
  const bool rising_edge = (!prev && current);
  prev = current;
  return rising_edge;
}

void VrSession::claim_inputs(const std::string& hand,
                             const std::string& component_id,
                             std::initializer_list<VrInput> inputs) {
  if (!is_valid_hand(hand)) {
    throw std::runtime_error(
      "VrSession::claim_inputs: hand must be 'left' or 'right', got '" +
      hand + "'");
  }
  if (component_id.empty()) {
    throw std::runtime_error(
      "VrSession::claim_inputs: component_id must not be empty");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  // First pass: conflict detection. Done before any mutation so a
  // partial claim never leaks on failure.
  for (const auto input : inputs) {
    const ClaimKey key{hand, input};
    auto it = claims_.find(key);
    if (it != claims_.end() && it->second != component_id) {
      throw std::runtime_error(
        std::string{"VrSession::claim_inputs: '"} +
        std::string{vr_input_name(input)} + "' on hand '" + hand +
        "' is already claimed by '" + it->second +
        "'; requested by '" + component_id + "'");
    }
  }
  // Second pass: apply. Idempotent when the same component re-claims.
  for (const auto input : inputs) {
    claims_[ClaimKey{hand, input}] = component_id;
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
