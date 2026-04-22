/**
 * @file keyboard_component.cpp
 * @brief Keyboard session-control hardware component.
 */

#include "trossen_sdk/hw/keyboard/keyboard_component.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#include "trossen_sdk/hw/hardware_registry.hpp"

namespace trossen::hw::keyboard {

namespace {

using Event   = session_control::SessionControlEvent;
using KeyPress = utils::KeyPress;

/// Parse a JSON key name into the utils::KeyPress enum. Throws on
/// unknown names so config errors fail loudly at configure() time.
KeyPress key_from_name(const std::string& name) {
  if (name == "left_arrow")  return KeyPress::kLeftArrow;
  if (name == "right_arrow") return KeyPress::kRightArrow;
  if (name == "up_arrow")    return KeyPress::kUpArrow;
  if (name == "down_arrow")  return KeyPress::kDownArrow;
  if (name == "space")       return KeyPress::kSpace;
  if (name == "enter")       return KeyPress::kEnter;
  if (name == "q")           return KeyPress::kQ;
  throw std::runtime_error("KeyboardComponent: unknown key name '" + name + "'");
}

/// Parse a JSON event name into the SessionControlEvent enum.
Event event_from_name(const std::string& name) {
  if (name == "start")        return Event::kStart;
  if (name == "stop_early")   return Event::kStopEarly;
  if (name == "rerecord")     return Event::kRerecord;
  if (name == "stop_session") return Event::kStopSession;
  throw std::runtime_error("KeyboardComponent: unknown event name '" + name + "'");
}

/// Defaults preserve the historical in-SessionManager arrow-key semantics.
std::unordered_map<KeyPress, Event> default_bindings() {
  return {
    {KeyPress::kLeftArrow,  Event::kRerecord},
    {KeyPress::kRightArrow, Event::kStart},
    {KeyPress::kQ,          Event::kStopSession},
  };
}

}  // namespace

KeyboardComponent::~KeyboardComponent() {
  // Must stop the reader thread before members destruct — otherwise
  // the thread may call into a freed callback.
  stop();
}

void KeyboardComponent::configure(const nlohmann::json& config) {
  bindings_ = default_bindings();

  if (config.contains("bindings")) {
    const auto& b = config.at("bindings");
    if (!b.is_object()) {
      throw std::runtime_error("KeyboardComponent: 'bindings' must be an object");
    }
    // A user-provided bindings block fully replaces the defaults —
    // otherwise removing a default (e.g. unbinding 'q') is impossible.
    bindings_.clear();
    for (const auto& [key_name, event_json] : b.items()) {
      if (!event_json.is_string()) {
        throw std::runtime_error(
          "KeyboardComponent: bindings['" + key_name + "'] must be a string");
      }
      bindings_[key_from_name(key_name)] =
        event_from_name(event_json.get<std::string>());
    }
  }

  if (config.contains("poll_interval_ms")) {
    const auto ms = config.at("poll_interval_ms").get<int>();
    if (ms <= 0) {
      throw std::runtime_error(
        "KeyboardComponent: 'poll_interval_ms' must be positive");
    }
    poll_interval_ = std::chrono::milliseconds{ms};
  }
}

nlohmann::json KeyboardComponent::get_info() const {
  return {
    {"type", get_type()},
    {"identifier", get_identifier()},
    {"poll_interval_ms", poll_interval_.count()},
    {"binding_count", bindings_.size()},
  };
}

void KeyboardComponent::set_callbacks(
  EventCallback on_event,
  DisconnectCallback on_disconnect)
{
  event_cb_      = std::move(on_event);
  disconnect_cb_ = std::move(on_disconnect);
}

void KeyboardComponent::start() {
  if (running_.exchange(true)) return;  // Already running.

  // Take raw-mode ownership for the lifetime of the reader thread.
  // SessionManager used to do this inline; with a KeyboardComponent
  // attached it's this component's responsibility.
  raw_mode_ = std::make_unique<utils::RawModeGuard>();

  stop_requested_.store(false);
  reader_thread_ = std::thread(&KeyboardComponent::reader_loop, this);
}

void KeyboardComponent::stop() {
  if (!running_.exchange(false)) return;  // Already stopped / never started.

  stop_requested_.store(true);
  if (reader_thread_.joinable()) {
    reader_thread_.join();
  }

  // Release raw mode after the thread exits so the final read() is not
  // interrupted mid-escape-sequence.
  raw_mode_.reset();
}

void KeyboardComponent::reader_loop() {
  // `poll_keypress` is non-blocking and polls stdin internally. Sleeping
  // between polls gives the shutdown path a bounded wake latency
  // (`poll_interval_`) without consuming CPU.
  while (!stop_requested_.load()) {
    const auto key = utils::poll_keypress();
    if (key != KeyPress::kNone) {
      auto it = bindings_.find(key);
      if (it != bindings_.end() && event_cb_) {
        event_cb_(it->second);
      }
    }
    std::this_thread::sleep_for(poll_interval_);
  }
}

REGISTER_HARDWARE(KeyboardComponent, "keyboard")

}  // namespace trossen::hw::keyboard
