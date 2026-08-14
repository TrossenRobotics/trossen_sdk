/**
 * @file glide_session.cpp
 * @brief Implementation of GlideSession input arbitration.
 */

#include "trossen_sdk/hw/glide/glide_session.hpp"

#include <sstream>
#include <stdexcept>
#include <vector>

namespace trossen::hw::glide {

std::string_view glide_input_name(GlideInput input) {
  switch (input) {
    case GlideInput::kJoystick: return "joystick";
    case GlideInput::kButton:   return "button";
    case GlideInput::kTrigger:  return "trigger";
  }
  return "unknown";
}

namespace {

/// Human-readable claim label for diagnostics, e.g. "button(3)" or "joystick".
std::string describe(const GlideClaim& claim) {
  std::ostringstream out;
  out << glide_input_name(claim.input);
  if (claim.input == GlideInput::kButton) {
    out << "(" << claim.bit << ")";
  }
  return out.str();
}

/// Buttons are addressed by bit position in a 32-bit mask, so a claim outside
/// that range can never match a real input and is rejected as a config error
/// rather than silently reserving nothing.
constexpr int kMaxButtonBit = 31;

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
  for (const auto& claim : claims) {
    if (claim.input == GlideInput::kButton &&
        (claim.bit < 0 || claim.bit > kMaxButtonBit)) {
      throw std::invalid_argument(
        "GlideSession::claim_inputs: button claim from '" + component_id +
        "' has bit " + std::to_string(claim.bit) + ", must be 0.." +
        std::to_string(kMaxButtonBit));
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);

  // Two passes for the same reason: detect every conflict before mutating, so a
  // rejected claim set leaves the table exactly as it was.
  for (const auto& claim : claims) {
    const ClaimKey key{arm_id, claim.input, claim.bit};
    auto it = claims_.find(key);
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
    it = (it->second == component_id) ? claims_.erase(it) : std::next(it);
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

    auto test_it = test_snapshots_.find(arm_id);
    if (test_it != test_snapshots_.end()) {
      return test_it->second;
    }

    auto it = readers_.find(arm_id);
    if (it == readers_.end()) return std::nullopt;
    // Copy the reader and invoke it after unlocking: a reader talks to hardware
    // and may block, and holding the session lock across that would let one
    // stalled handle freeze every other component's claims and reads.
    reader = it->second;
  }
  return reader ? reader() : std::nullopt;
}

void GlideSession::set_test_snapshot(const std::string& arm_id,
                                     const GlideInputSnapshot& snapshot) {
  std::lock_guard<std::mutex> lock(mutex_);
  test_snapshots_[arm_id] = snapshot;
}

void GlideSession::clear_test_snapshots() {
  std::lock_guard<std::mutex> lock(mutex_);
  test_snapshots_.clear();
}

void GlideSession::register_writer(const std::string& arm_id, GlideOutputWriter writer) {
  GlideOutputWriter to_push;
  GlideOutputCommand staged;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    writers_[arm_id] = std::move(writer);

    // A component may have staged its LEDs before the handle's driver existed
    // (components configure in declaration order, and the arm input adapter
    // comes after the base). Push the staged state now so the ordering does not
    // decide whether the lights come on.
    auto it = outputs_.find(arm_id);
    if (it != outputs_.end() && it->second != GlideOutputCommand{}) {
      to_push = writers_[arm_id];
      staged  = it->second;
    }
  }
  if (to_push) to_push(staged);
}

void GlideSession::unregister_writer(const std::string& arm_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  writers_.erase(arm_id);
}

GlideOutputCommand GlideSession::output_command(const std::string& arm_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = outputs_.find(arm_id);
  return it == outputs_.end() ? GlideOutputCommand{} : it->second;
}

bool GlideSession::apply_output(const std::string& arm_id,
                                const std::function<void(GlideOutputCommand&)>& mutate) {
  GlideOutputWriter writer;
  GlideOutputCommand command;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    GlideOutputCommand& staged = outputs_[arm_id];
    const GlideOutputCommand before = staged;
    mutate(staged);
    // Unchanged means nothing to send. This is what lets a poll loop call in
    // every tick: only an actual edge reaches the wire.
    if (staged == before) return true;

    auto it = writers_.find(arm_id);
    if (it == writers_.end()) return true;  // staged for whenever a writer lands
    // Copy and invoke outside the lock, exactly as read_inputs does: a write
    // talks to hardware and may block.
    writer  = it->second;
    command = staged;
  }
  return writer ? writer(command) : true;
}

bool GlideSession::set_button_led(const std::string& arm_id, int bit, GlideLedEffect effect) {
  if (bit < 0 || bit >= kGlideButtonCount) return true;
  return apply_output(arm_id, [bit, effect](GlideOutputCommand& cmd) {
    cmd.led_effects[static_cast<std::size_t>(bit)] = effect;
  });
}

bool GlideSession::set_led_brightness(const std::string& arm_id, std::uint8_t brightness) {
  return apply_output(arm_id, [brightness](GlideOutputCommand& cmd) {
    cmd.led_brightness = brightness;
  });
}

void GlideSession::reset_for_test() {
  std::lock_guard<std::mutex> lock(mutex_);
  claims_.clear();
  readers_.clear();
  test_snapshots_.clear();
  writers_.clear();
  outputs_.clear();
}

}  // namespace trossen::hw::glide
