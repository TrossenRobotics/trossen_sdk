/**
 * @file glide_session.cpp
 * @brief Implementation of GlideSession input arbitration.
 */

#include "trossen_sdk/hw/glide/glide_session.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace trossen::hw::glide {

std::string_view glide_input_name(GlideInput input) {
  switch (input) {
    case GlideInput::kJoystick: return "joystick";
    case GlideInput::kButton:   return "button";
  }
  return "unknown";
}

namespace {

/// Human-readable claim label for diagnostics, e.g. "button(3)" or "joystick".
std::string describe(const GlideClaim& claim) {
  std::string out(glide_input_name(claim.input));
  if (claim.input == GlideInput::kButton) {
    out += "(" + std::to_string(claim.bit) + ")";
  }
  return out;
}

}  // namespace

GlideSession& GlideSession::instance() {
  // Function-local static: constructed on first use, so a component
  // constructed during static init cannot observe a half-built session.
  static GlideSession session;
  return session;
}

void GlideSession::claim_inputs(const std::string& arm_id,
                                const std::string& component_id,
                                std::initializer_list<GlideClaim> claims) {
  if (arm_id.empty()) {
    throw std::invalid_argument("GlideSession::claim_inputs: arm_id must not be empty");
  }
  if (component_id.empty()) {
    throw std::invalid_argument("GlideSession::claim_inputs: component_id must not be empty");
  }

  // Validate every claim before touching the table, so a bad claim late in the
  // list cannot leave the earlier ones half-applied.
  //
  // Bounded by the number of physical buttons, not by the width of the bitmask
  // they arrive in. The mask is 32 bits, so a claim on bit 7 would otherwise
  // succeed and then read false forever — a config typo that presents as a
  // button which simply never works.
  for (const auto& claim : claims) {
    if (claim.input == GlideInput::kButton &&
        (claim.bit < 0 || claim.bit >= kGlideButtonCount)) {
      throw std::invalid_argument(
        "GlideSession::claim_inputs: button claim from '" + component_id +
        "' has bit " + std::to_string(claim.bit) + ", must be 0.." +
        std::to_string(kGlideButtonCount - 1));
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);

  // Two passes for the same reason: detect every conflict before mutating, so a
  // rejected claim set leaves the table exactly as it was.
  for (const auto& claim : claims) {
    auto it = claims_.find(ClaimKey{arm_id, claim.input, claim.bit});
    if (it != claims_.end() && it->second != component_id) {
      throw std::runtime_error(
        "GlideSession: input " + describe(claim) + " on handle '" + arm_id +
        "' is already claimed by '" + it->second + "', so '" + component_id +
        "' cannot also read it. Two components reading one input would each act "
        "on the operator's input without knowing about the other; give them "
        "different inputs, or one component both roles.");
    }
  }

  for (const auto& claim : claims) {
    claims_[ClaimKey{arm_id, claim.input, claim.bit}] = component_id;
  }
}

void GlideSession::release_claims(const std::string& component_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = claims_.begin(); it != claims_.end();) {
    if (it->second == component_id) {
      it = claims_.erase(it);
    } else {
      ++it;
    }
  }
}

std::optional<std::string> GlideSession::claim_holder(const std::string& arm_id,
                                                     const GlideClaim& claim) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = claims_.find(ClaimKey{arm_id, claim.input, claim.bit});
  if (it == claims_.end()) return std::nullopt;
  return it->second;
}

void GlideSession::register_reader(const std::string& arm_id, GlideInputReader reader) {
  std::lock_guard<std::mutex> lock(mutex_);
  readers_[arm_id] = std::move(reader);
}

void GlideSession::unregister_reader(const std::string& arm_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  readers_.erase(arm_id);
}

std::optional<GlideInputSnapshot> GlideSession::read_inputs(const std::string& arm_id) const {
  GlideInputReader reader;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = readers_.find(arm_id);
    if (it == readers_.end()) return std::nullopt;
    // Copy the reader and invoke it after unlocking: a reader talks to hardware
    // and may block, and holding the session lock across that would let one
    // stalled handle freeze every other component's claims and reads.
    reader = it->second;
  }
  return reader ? reader() : std::nullopt;
}

}  // namespace trossen::hw::glide
