/**
 * @file vr_session_control.cpp
 * @brief VR button → session-control event bridge.
 */

#include "trossen_sdk/hw/vr/vr_session_control.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "trossen_sdk/hw/hardware_registry.hpp"

namespace trossen::hw::vr {

namespace {

using Event = session_control::SessionControlEvent;

/// Parse a JSON input name into the VrInput enum. Only button-like
/// inputs are valid here — pose / thumbstick don't map to discrete
/// session events.
VrInput vr_input_from_name(const std::string& name) {
  if (name == "button_a") return VrInput::kButtonA;
  if (name == "button_b") return VrInput::kButtonB;
  if (name == "menu")     return VrInput::kMenu;
  if (name == "trigger")  return VrInput::kTrigger;
  if (name == "grip")     return VrInput::kGrip;
  throw std::runtime_error(
    "VrSessionControlComponent: input '" + name +
    "' is not bindable for session control (valid: button_a, button_b, "
    "menu, trigger, grip)");
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
    {VrInput::kButtonA, Event::kStart},
    {VrInput::kButtonB, Event::kRerecord},
    {VrInput::kGrip,    Event::kStopSession},
  };
}

}  // namespace

VrSessionControlComponent::~VrSessionControlComponent() {
  // Stop the reader thread before anything else so callbacks stop
  // firing into a half-destructed object.
  stop();
  if (session_held_) {
    VrSession::instance().release_claims(get_identifier());
    VrSession::instance().release();
    session_held_ = false;
  }
}

void VrSessionControlComponent::configure(const nlohmann::json& config) {
  if (!config.contains("controller")) {
    throw std::runtime_error(
      "VrSessionControlComponent: 'controller' is required "
      "(\"left\" or \"right\")");
  }
  controller_ = config.at("controller").get<std::string>();
  if (controller_ != "left" && controller_ != "right") {
    throw std::runtime_error(
      "VrSessionControlComponent: 'controller' must be \"left\" or "
      "\"right\", got \"" + controller_ + "\"");
  }

  vr_port_ = config.value("vr_port", static_cast<std::uint16_t>(5432));

  const double wait_s = config.value("wait_for_quest_s", 10.0);
  if (!std::isfinite(wait_s) || wait_s < 0.0) {
    throw std::runtime_error(
      "VrSessionControlComponent: 'wait_for_quest_s' must be a "
      "non-negative finite number");
  }
  wait_for_quest_ = std::chrono::milliseconds(
    static_cast<std::int64_t>(wait_s * 1000.0));

  const int poll_ms = config.value("poll_interval_ms", 50);
  if (poll_ms <= 0) {
    throw std::runtime_error(
      "VrSessionControlComponent: 'poll_interval_ms' must be positive");
  }
  poll_interval_ = std::chrono::milliseconds{poll_ms};

  analog_threshold_ = config.value("analog_threshold", 0.5);
  if (!std::isfinite(analog_threshold_) ||
      analog_threshold_ <= 0.0 || analog_threshold_ >= 1.0) {
    throw std::runtime_error(
      "VrSessionControlComponent: 'analog_threshold' must be in (0, 1)");
  }

  const double disconnect_s = config.value("disconnect_timeout_s", 2.0);
  if (!std::isfinite(disconnect_s) || disconnect_s <= 0.0) {
    throw std::runtime_error(
      "VrSessionControlComponent: 'disconnect_timeout_s' must be "
      "positive and finite");
  }
  disconnect_timeout_ = std::chrono::milliseconds(
    static_cast<std::int64_t>(disconnect_s * 1000.0));

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
      bindings_.push_back(Binding{
        vr_input_from_name(input_name),
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
  // configured hand. Conflicts with arm/base/other session-control
  // components surface here as readable errors.
  VrSession::instance().ensure_started(vr_port_);
  session_held_ = true;

  std::vector<VrInput> claimed;
  claimed.reserve(bindings_.size());
  for (const auto& b : bindings_) claimed.push_back(b.input);

  // `claim_inputs` takes an initializer_list; build one via a two-step
  // pass is overkill — use a small stack buffer via the vector range.
  // Easiest: loop per-binding so an error message names the specific
  // input that conflicted.
  auto& session = VrSession::instance();
  for (const auto input : claimed) {
    session.claim_inputs(controller_, get_identifier(), {input});
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
    {"controller",       controller_},
    {"vr_port",          vr_port_},
    {"poll_interval_ms", poll_interval_.count()},
    {"binding_count",    bindings_.size()},
    {"connected",        VrSession::instance().is_quest_connected()},
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

  // Block until the Quest app connects. Throws if the user forgot to
  // launch the VR app / mDNS helper — fails loud, early, and clearly.
  if (!VrSession::instance().wait_for_connection(wait_for_quest_)) {
    running_.store(false);
    throw std::runtime_error(
      "VrSessionControlComponent: timed out waiting for Meta Quest to "
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

std::string VrSessionControlComponent::input_to_key(VrInput input) const {
  // The VR protocol uses `<hand>_<button>` keys (e.g. "right_a").
  switch (input) {
    case VrInput::kButtonA: return controller_ + "_a";
    case VrInput::kButtonB: return controller_ + "_b";
    case VrInput::kMenu:    return controller_ + "_menu";
    case VrInput::kTrigger: return controller_ + "_trigger";
    case VrInput::kGrip:    return controller_ + "_grip";
    default:                return "";
  }
}

bool VrSessionControlComponent::is_digital(VrInput input) {
  switch (input) {
    case VrInput::kButtonA:
    case VrInput::kButtonB:
    case VrInput::kMenu:
      return true;
    case VrInput::kTrigger:
    case VrInput::kGrip:
      return false;
    default:
      return false;
  }
}

void VrSessionControlComponent::reader_loop() {
  auto& session = VrSession::instance();

  // Sequence staleness tracking: once a frame is observed, a lack of
  // sequence change for `disconnect_timeout_` is treated as a
  // permanent disconnect. Wait until we see the first frame before
  // arming the watchdog — the initial connection may still be warming
  // up even though `wait_for_connection()` returned true.
  std::uint64_t last_seq{0};
  bool          has_first_seq{false};
  auto          last_seq_change = std::chrono::steady_clock::now();
  bool          disconnect_fired = false;

  while (!stop_requested_.load()) {
    const auto frame_opt = session.latest_frame();
    const auto now       = std::chrono::steady_clock::now();

    if (frame_opt) {
      const auto& frame = *frame_opt;

      // Disconnect watchdog — based on frame-sequence progress, not
      // `is_quest_connected()` (which never goes false once set).
      if (!has_first_seq || frame.sequence != last_seq) {
        last_seq        = frame.sequence;
        has_first_seq   = true;
        last_seq_change = now;
      } else if (has_first_seq &&
                 (now - last_seq_change) > disconnect_timeout_ &&
                 !disconnect_fired &&
                 disconnect_cb_) {
        disconnect_fired = true;
        disconnect_cb_();
        // Keep looping so stop() can still join cleanly; the callback
        // ran once and must not fire again.
      }

      // Rising-edge detection per bound input.
      for (const auto& b : bindings_) {
        const std::string key = input_to_key(b.input);
        const auto it = frame.buttons.find(key);
        bool pressed = false;
        if (it != frame.buttons.end()) {
          if (is_digital(b.input)) {
            if (const bool* v = std::get_if<bool>(&it->second)) {
              pressed = *v;
            }
          } else {
            if (const double* d = std::get_if<double>(&it->second)) {
              pressed = (*d >= analog_threshold_);
            } else if (const bool* v = std::get_if<bool>(&it->second)) {
              // Digital trigger variant — treat press as above-threshold.
              pressed = *v;
            }
          }
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
