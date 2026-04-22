/**
 * @file vr_base_joystick.cpp
 * @brief Implementation of the Meta Quest Base-space teleop leader.
 */

#include "trossen_sdk/hw/vr/vr_base_joystick.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <variant>

#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/vr/vr_session.hpp"

namespace trossen::hw::vr {

namespace {

/// Extract an analog thumbstick axis from a VR frame. The protocol ships
/// thumbstick axes as `{left,right}_thumbstick_{x,y}` doubles in `[-1, 1]`.
double read_axis(
    const trossen_vr::VRState& frame, const std::string& key) {
  const auto it = frame.buttons.find(key);
  if (it == frame.buttons.end()) return 0.0;
  if (const double* d = std::get_if<double>(&it->second)) return *d;
  return 0.0;
}

}  // namespace

double VrBaseJoystickComponent::apply_deadzone(double v, double deadzone) {
  const double a = std::fabs(v);
  if (a <= deadzone) return 0.0;
  // Rescale so the effective travel spans the whole [-1, 1] range starting
  // at the deadzone boundary — without this the stick would feel dead up
  // to `deadzone` and then leap to that fraction of full range.
  const double span = 1.0 - deadzone;
  const double sign = (v > 0.0) ? 1.0 : -1.0;
  return sign * std::clamp((a - deadzone) / span, 0.0, 1.0);
}

VrBaseJoystickComponent::~VrBaseJoystickComponent() {
  if (session_held_) {
    VrSession::instance().release_claims(get_identifier());
    VrSession::instance().release();
    session_held_ = false;
  }
}

void VrBaseJoystickComponent::configure(const nlohmann::json& config) {
  if (!config.contains("controller")) {
    throw std::runtime_error(
      "VrBaseJoystickComponent: 'controller' is required (\"left\" or \"right\")");
  }
  controller_ = config.at("controller").get<std::string>();
  if (controller_ != "left" && controller_ != "right") {
    throw std::runtime_error(
      "VrBaseJoystickComponent: 'controller' must be \"left\" or \"right\", got \"" +
      controller_ + "\"");
  }

  vr_port_         = config.value("vr_port",         static_cast<std::uint16_t>(5432));
  max_linear_mps_  = config.value("max_linear_mps",  0.5);
  max_angular_rps_ = config.value("max_angular_rps", 1.0);
  deadzone_        = config.value("deadzone",        0.1);
  if (deadzone_ < 0.0 || deadzone_ >= 1.0) {
    throw std::runtime_error(
      "VrBaseJoystickComponent: 'deadzone' must be in [0, 1)");
  }

  const double wait_s = config.value("wait_for_quest_s", 10.0);
  if (!std::isfinite(wait_s) || wait_s < 0.0) {
    throw std::runtime_error(
      "VrBaseJoystickComponent: 'wait_for_quest_s' must be a non-negative number");
  }
  wait_for_quest_ = std::chrono::milliseconds(
    static_cast<std::int64_t>(wait_s * 1000.0));

  VrSession::instance().ensure_started(vr_port_);
  session_held_ = true;

  // Thumbstick is the only input this component consumes.
  VrSession::instance().claim_inputs(
    controller_, get_identifier(), {VrInput::kThumbstick});
}

nlohmann::json VrBaseJoystickComponent::get_info() const {
  return nlohmann::json{
    {"type",             get_type()},
    {"identifier",       get_identifier()},
    {"controller",       controller_},
    {"vr_port",          vr_port_},
    {"max_linear_mps",   max_linear_mps_},
    {"max_angular_rps",  max_angular_rps_},
    {"deadzone",         deadzone_},
    {"connected",        VrSession::instance().is_quest_connected()},
  };
}

void VrBaseJoystickComponent::prepare_for_teleop() {
  if (!VrSession::instance().wait_for_connection(wait_for_quest_)) {
    throw std::runtime_error(
      "VrBaseJoystickComponent: timed out waiting for Meta Quest to connect "
      "on port " + std::to_string(vr_port_) +
      " — is the VR app running?");
  }
}

void VrBaseJoystickComponent::end_teleop() {
  if (session_held_) {
    VrSession::instance().release();
    session_held_ = false;
  }
}

std::vector<float> VrBaseJoystickComponent::read() {
  const auto frame = VrSession::instance().latest_frame();
  if (!frame) return {0.0f, 0.0f};

  const std::string y_key = controller_ + "_thumbstick_y";
  const std::string x_key = controller_ + "_thumbstick_x";
  const double y = apply_deadzone(read_axis(*frame, y_key), deadzone_);
  const double x = apply_deadzone(read_axis(*frame, x_key), deadzone_);

  // Forward stick → forward velocity; left stick → positive yaw (CCW about
  // the vertical axis), so we negate x. `max_*` caps are applied after the
  // deadzone rescale so full-deflection matches the user-configured limit.
  const double linear  =  y * max_linear_mps_;
  const double angular = -x * max_angular_rps_;
  return {static_cast<float>(linear), static_cast<float>(angular)};
}

void VrBaseJoystickComponent::write(const std::vector<float>& /*cmd*/) {
  // Leader only.
}

REGISTER_HARDWARE(VrBaseJoystickComponent, "vr_base_joystick")

}  // namespace trossen::hw::vr
