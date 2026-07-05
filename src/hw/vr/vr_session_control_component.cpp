/**
 * @file vr_session_control_component.cpp
 * @brief VR button → session-control event bridge.
 */

#include "trossen_sdk/hw/vr/vr_session_control_component.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "trossen_sdk/hw/hardware_registry.hpp"

namespace trossen::hw::vr {

namespace {

using Event = session_control::SessionControlEvent;

/// Parse a JSON input name into the VrInput enum. Only button
/// inputs are valid here.
///
/// Valid inputs:
///   - button_a / button_b : primary/secondary button on right controller (A/B)
///   - button_x / button_y : primary/secondary button on left controller (X/Y)
///   button_x is an alias for button_a; button_y is an alias for button_b.
///   Both map to the same underlying VrInput because VRFrame stores buttons
///   as one/two regardless of which hand — the controller_type determines
///   which physical button is read at runtime.
VrInput vr_input_from_name(const std::string& name) {
  if (name == "button_a" || name == "button_x") return VrInput::kButtonOne;
  if (name == "button_b" || name == "button_y") return VrInput::kButtonTwo;
  throw std::runtime_error(
    "VrSessionControlComponent: input '" + name +
    "' is not bindable for session control "
    "(valid: button_a, button_b, button_x, button_y)");
}

Event event_from_name(const std::string& name) {
  if (name == "start")        return Event::kStart;
  if (name == "stop_early")   return Event::kStopEarly;
  if (name == "rerecord")     return Event::kRerecord;
  if (name == "stop_session") return Event::kStopSession;
  throw std::runtime_error(
    "VrSessionControlComponent: unknown event name '" + name + "'");
}

std::vector<VrSessionControlComponent::Binding> default_bindings() {
  return {
    {VrInput::kButtonOne, Event::kStart},
    {VrInput::kButtonTwo, Event::kRerecord},
  };
}

}  // namespace

VrSessionControlComponent::~VrSessionControlComponent() {
  // Stop the reader thread first so callbacks stop firing into a
  // half-destructed object; the session lease releases on destruction.
  stop();
}

void VrSessionControlComponent::configure(const nlohmann::json& config) {
  if (!config.contains("controller_type")) {
    throw std::runtime_error(
      "VrSessionControlComponent: 'controller_type' is required "
      "(\"left\" or \"right\")");
  }
  controller_type_ = config.at("controller_type").get<std::string>();
  if (controller_type_ != "left" && controller_type_ != "right") {
    throw std::runtime_error(
      "VrSessionControlComponent: 'controller_type' must be \"left\" or "
      "\"right\", got \"" + controller_type_ + "\"");
  }

  vr_port_ = config.value("vr_port", static_cast<std::uint16_t>(9000));

  const double wait_s = config.value("connection_timeout_s", 10.0);
  if (!std::isfinite(wait_s) || wait_s < 0.0) {
    throw std::runtime_error(
      "VrSessionControlComponent: 'connection_timeout_s' must be a "
      "non-negative finite number");
  }
  connection_timeout_ = std::chrono::milliseconds(
    static_cast<std::int64_t>(wait_s * 1000.0));

  const int poll_ms = config.value("poll_interval_ms", 50);
  if (poll_ms <= 0) {
    throw std::runtime_error(
      "VrSessionControlComponent: 'poll_interval_ms' must be positive");
  }
  poll_interval_ = std::chrono::milliseconds{poll_ms};

  // Parse bindings. A user-provided `bindings` block fully replaces
  // the defaults — otherwise unbinding a default entry is impossible.
  bindings_.clear();
  if (config.contains("bindings")) {
    const auto& b = config.at("bindings");
    if (!b.is_object()) {
      throw std::runtime_error(
        "VrSessionControlComponent: 'bindings' must be an object");
    }
    for (const auto& [input_name, event_json] : b.items()) {
      if (!event_json.is_string()) {
        throw std::runtime_error(
          "VrSessionControlComponent: bindings['" + input_name +
          "'] must be a string");
      }
      const VrInput input = vr_input_from_name(input_name);
      // button_a/button_x and button_b/button_y are aliases for the same
      // physical button. Reject configs that bind both aliases, which would
      // otherwise fire the session-control event twice per press.
      for (const auto& existing : bindings_) {
        if (existing.input == input) {
          throw std::runtime_error(
            "VrSessionControlComponent: bindings['" + input_name +
            "'] maps to the same button as an earlier binding "
            "(button_a/button_x and button_b/button_y are aliases); "
            "bind each button at most once");
        }
      }
      bindings_.push_back(Binding{
        input,
        event_from_name(event_json.get<std::string>())});
    }
  } else {
    bindings_ = default_bindings();
  }

  if (bindings_.empty()) {
    throw std::runtime_error(
      "VrSessionControlComponent: at least one binding is required");
  }

  // Acquire the shared VR session and claim the bound inputs on the
  // configured controller type. Conflicts with arm/base/other session-control
  // components surface here as readable errors.
  session_lease_.acquire(vr_port_, get_identifier());

  // Claim one input at a time so a conflict error names the specific button
  // that clashed rather than the whole set.
  auto& session = VrSession::instance();
  for (const auto& b : bindings_) {
    session.claim_inputs(controller_type_, get_identifier(), {b.input});
  }

  // Reset rising-edge state for any subsequent start().
  prev_pressed_.clear();
  for (const auto& b : bindings_) {
    prev_pressed_[b.input] = false;
  }
}

nlohmann::json VrSessionControlComponent::get_info() const {
  nlohmann::json j{
    {"type",             get_type()},
    {"identifier",       get_identifier()},
    {"controller_type",  controller_type_},
    {"vr_port",          vr_port_},
    {"poll_interval_ms", poll_interval_.count()},
    {"binding_count",    bindings_.size()},
    {"connected",        VrSession::instance().is_vr_connected()},
  };
  return j;
}

void VrSessionControlComponent::set_callbacks(
  EventCallback on_event,
  DisconnectCallback on_disconnect)
{
  event_cb_      = std::move(on_event);
  disconnect_cb_ = std::move(on_disconnect);
}

void VrSessionControlComponent::start() {
  if (running_.exchange(true)) return;  // Already running.

  // Block until the VR headset connects. Throws if the headset app is
  // not running — fails early with a clear message.
  if (!VrSession::instance().wait_for_connection(connection_timeout_)) {
    running_.store(false);
    throw std::runtime_error(
      "VrSessionControlComponent: timed out waiting for VR headset to "
      "connect on port " + std::to_string(vr_port_) +
      " — is the VR app running?");
  }

  stop_requested_.store(false);
  reader_thread_ = std::thread(&VrSessionControlComponent::reader_loop, this);
}

void VrSessionControlComponent::stop() {
  if (!running_.exchange(false)) return;  // Already stopped / never started.
  stop_requested_.store(true);
  if (reader_thread_.joinable()) {
    reader_thread_.join();
  }
}

void VrSessionControlComponent::reader_loop() {
  auto& session = VrSession::instance();
  bool disconnect_fired = false;

  while (!stop_requested_.load()) {
    // Use the session's connection state to detect drops: latest_frame()
    // keeps returning the last frame after a disconnect, so it can never
    // signal one on its own.
    if (!session.is_vr_connected()) {
      if (!disconnect_fired && disconnect_cb_) {
        disconnect_fired = true;
        disconnect_cb_();
      }
      std::this_thread::sleep_for(poll_interval_);
      continue;
    }
    disconnect_fired = false;

    const auto frame_opt = session.latest_frame();
    if (frame_opt) {
      const auto& frame = *frame_opt;

      const auto& controller = (controller_type_ == "right")
        ? frame.right_controller
        : frame.left_controller;

      // Rising-edge detection per bound input: fire once per press.
      for (const auto& b : bindings_) {
        bool pressed = false;

        switch (b.input) {
          case VrInput::kButtonOne:
            // Primary button: A on the right controller, X on the left.
            pressed = (controller.buttons.one != 0);
            break;
          case VrInput::kButtonTwo:
            // Secondary button: B on the right controller, Y on the left.
            pressed = (controller.buttons.two != 0);
            break;
          default:
            pressed = false;
            break;
        }

        const bool was = prev_pressed_[b.input];
        if (pressed && !was && event_cb_) {
          event_cb_(b.event);
        }
        prev_pressed_[b.input] = pressed;
      }
    }

    std::this_thread::sleep_for(poll_interval_);
  }
}

REGISTER_HARDWARE(VrSessionControlComponent, "vr_session_control")

}  // namespace trossen::hw::vr
