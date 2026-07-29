/**
 * @file glide_session_control_component.cpp
 * @brief Implementation of GlideSessionControlComponent.
 */

#include "trossen_sdk/hw/glide/glide_session_control_component.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

#include "trossen_sdk/hw/hardware_registry.hpp"

namespace trossen::hw::glide {

using session_control::SessionControlEvent;

namespace {

/// Parse a config event name into the intent enum. `kNone` is deliberately not
/// accepted: binding a button to "no event" is always a mistake, not a way to
/// disable it (leave the entry out instead).
SessionControlEvent event_from_name(const std::string& name) {
  if (name == "start")        return SessionControlEvent::kStart;
  if (name == "stop_early")   return SessionControlEvent::kStopEarly;
  if (name == "rerecord")     return SessionControlEvent::kRerecord;
  if (name == "stop_session") return SessionControlEvent::kStopSession;
  throw std::invalid_argument(
    "GlideSessionControlComponent: unknown event '" + name +
    "' (expected start, stop_early, rerecord, or stop_session)");
}

std::string event_name(SessionControlEvent event) {
  switch (event) {
    case SessionControlEvent::kStart:       return "start";
    case SessionControlEvent::kStopEarly:   return "stop_early";
    case SessionControlEvent::kRerecord:    return "rerecord";
    case SessionControlEvent::kStopSession: return "stop_session";
    case SessionControlEvent::kNone:        return "none";
  }
  return "unknown";
}

}  // namespace

GlideSessionControlComponent::~GlideSessionControlComponent() {
  // Stop the poller before the lease member destructs and drops the claims:
  // the thread reads inputs this component has claimed, so it must be joined
  // while those claims are still valid.
  stop();
}

void GlideSessionControlComponent::configure(const nlohmann::json& config) {
  if (!config.contains("buttons") || !config.at("buttons").is_array() ||
      config.at("buttons").empty()) {
    throw std::invalid_argument(
      "GlideSessionControlComponent '" + get_identifier() +
      "': config requires a non-empty 'buttons' array; with none bound the "
      "component would claim nothing and never emit an event");
  }

  poll_rate_hz_ = config.value("poll_rate_hz", 50.0);
  if (!std::isfinite(poll_rate_hz_) || poll_rate_hz_ <= 0.0) {
    throw std::invalid_argument(
      "GlideSessionControlComponent: 'poll_rate_hz' must be finite and positive");
  }

  const auto debounce_ms = config.value("debounce_ms", 40);
  if (debounce_ms < 0) {
    throw std::invalid_argument(
      "GlideSessionControlComponent: 'debounce_ms' must not be negative");
  }
  debounce_ = std::chrono::milliseconds(debounce_ms);

  // Parse everything before claiming, so a bad entry cannot leave this
  // component holding buttons it never bound.
  std::vector<ButtonBinding> parsed;
  parsed.reserve(config.at("buttons").size());

  for (const auto& j : config.at("buttons")) {
    if (!j.contains("arm_id") || !j.contains("bit") || !j.contains("event")) {
      throw std::invalid_argument(
        "GlideSessionControlComponent: each 'buttons' entry requires arm_id, "
        "bit, and event");
    }
    ButtonBinding binding;
    binding.arm_id = j.at("arm_id").get<std::string>();
    binding.bit    = j.at("bit").get<int>();
    binding.event  = event_from_name(j.at("event").get<std::string>());
    parsed.push_back(std::move(binding));
  }

  GlideClaimLease lease;
  for (const auto& binding : parsed) {
    lease.add(binding.arm_id, get_identifier(), {glide_button(binding.bit)});
  }

  buttons_ = std::move(parsed);
  lease_   = std::move(lease);
}

void GlideSessionControlComponent::start() {
  if (running_.exchange(true)) return;  // already started
  poll_thread_ = std::thread(&GlideSessionControlComponent::poll_loop, this);
}

void GlideSessionControlComponent::stop() {
  if (!running_.exchange(false)) return;  // already stopped
  if (poll_thread_.joinable()) poll_thread_.join();
}

void GlideSessionControlComponent::poll_loop() {
  const auto period = std::chrono::duration<double>(1.0 / poll_rate_hz_);
  auto next_tick = std::chrono::steady_clock::now();
  while (running_.load(std::memory_order_relaxed)) {
    poll_once();
    next_tick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
    std::this_thread::sleep_until(next_tick);
  }
}

void GlideSessionControlComponent::poll_once() {
  const auto now = std::chrono::steady_clock::now();

  for (auto& binding : buttons_) {
    const auto snapshot = GlideSession::instance().read_inputs(binding.arm_id);
    if (!snapshot) continue;  // handle unavailable; nothing to compare against

    const bool pressed = snapshot->button(binding.bit);

    if (pressed == binding.was_pressed) {
      // Matches the confirmed state — any in-flight change was a transient.
      binding.pending = false;
      continue;
    }

    if (!binding.pending) {
      binding.pending    = true;
      binding.changed_at = now;
      // A zero debounce window accepts immediately, which also makes
      // poll_once() usable as a single deterministic step in tests.
      if (debounce_.count() > 0) continue;
    } else if (now - binding.changed_at < debounce_) {
      continue;  // change hasn't held long enough to be real
    }

    binding.was_pressed = pressed;
    binding.pending     = false;

    // Rising edge only: a held button is one intent, and release is not an
    // event of its own.
    if (pressed) emit_event(binding.event);
  }
}

nlohmann::json GlideSessionControlComponent::get_info() const {
  nlohmann::json buttons = nlohmann::json::array();
  for (const auto& binding : buttons_) {
    buttons.push_back({
      {"arm_id", binding.arm_id},
      {"bit", binding.bit},
      {"event", event_name(binding.event)},
    });
  }
  return {
    {"type", get_type()},
    {"poll_rate_hz", poll_rate_hz_},
    {"debounce_ms", debounce_.count()},
    {"buttons", buttons},
  };
}

REGISTER_HARDWARE(GlideSessionControlComponent, "glide_session_control")

}  // namespace trossen::hw::glide
