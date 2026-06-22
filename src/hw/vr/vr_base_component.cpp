/**
 * @file vr_base_component.cpp
 * @brief Implementation of the VR thumbstick base-velocity teleop leader.
 */

#include "trossen_sdk/hw/vr/vr_base_component.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/vr/vr_session.hpp"

namespace trossen::hw::vr {

namespace {

/// Rescale `v` from `[-1, 1]` with a centered deadzone:
/// returns 0 when |v| <= deadzone, otherwise linearly maps
/// the remaining travel to the full [-1, 1] range.
double apply_deadzone(double v, double deadzone) {
  const double a = std::fabs(v);
  if (a <= deadzone) return 0.0;
  const double span = 1.0 - deadzone;
  const double sign = (v > 0.0) ? 1.0 : -1.0;
  return sign * std::clamp((a - deadzone) / span, 0.0, 1.0);
}

}  // namespace

VrBaseComponent::~VrBaseComponent() {
  if (session_held_) {
    VrSession::instance().release_claims(get_identifier());
    VrSession::instance().release();
    session_held_ = false;
  }
}

void VrBaseComponent::configure(const nlohmann::json& config) {
  // Two accepted shapes:
  //   * `controller_type` alone → both axes from that controller.
  //   * `linear_controller_type` and/or `angular_controller_type` : per-axis
  //     split mode.
  const bool has_linear  = config.contains("linear_controller_type");
  const bool has_angular = config.contains("angular_controller_type");
  const bool has_legacy  = config.contains("controller_type");

  if (!has_linear && !has_angular && !has_legacy) {
    throw std::runtime_error(
      "VrBaseComponent: one of 'controller_type', "
      "'linear_controller_type', or 'angular_controller_type' is required");
  }

  const std::string fallback = has_legacy
    ? config.at("controller_type").get<std::string>()
    : std::string{"left"};

  linear_controller_type_  = has_linear
    ? config.at("linear_controller_type").get<std::string>()
    : fallback;
  angular_controller_type_ = has_angular
    ? config.at("angular_controller_type").get<std::string>()
    : fallback;

  auto validate = [](const char* field, const std::string& v) {
    if (v != "left" && v != "right") {
      throw std::runtime_error(
        std::string{"VrBaseComponent: '"} + field +
        "' must be \"left\" or \"right\", got \"" + v + "\"");
    }
  };
  validate("linear_controller_type",  linear_controller_type_);
  validate("angular_controller_type", angular_controller_type_);

  vr_port_         = config.value("vr_port",         static_cast<std::uint16_t>(9000));
  max_linear_mps_  = config.value("max_linear_mps",  0.5);
  max_angular_rps_ = config.value("max_angular_rps", 1.0);
  if (!std::isfinite(max_linear_mps_) || max_linear_mps_ < 0.0) {
    throw std::runtime_error(
      "VrBaseComponent: 'max_linear_mps' must be a non-negative finite number");
  }
  if (!std::isfinite(max_angular_rps_) || max_angular_rps_ < 0.0) {
    throw std::runtime_error(
      "VrBaseComponent: 'max_angular_rps' must be a non-negative finite number");
  }
  deadzone_        = config.value("deadzone",        0.1);
  if (deadzone_ < 0.0 || deadzone_ >= 1.0) {
    throw std::runtime_error(
      "VrBaseComponent: 'deadzone' must be in [0, 1)");
  }

  const double wait_s = config.value("connection_timeout_s", 10.0);
  if (!std::isfinite(wait_s) || wait_s < 0.0) {
    throw std::runtime_error(
      "VrBaseComponent: 'connection_timeout_s' must be a non-negative number");
  }
  connection_timeout_ = std::chrono::milliseconds(
    static_cast<std::int64_t>(wait_s * 1000.0));

  VrSession::instance().ensure_started(vr_port_);
  session_held_ = true;

  // Claim the thumbstick on each controller type we read from.
  VrSession::instance().claim_inputs(
    linear_controller_type_, get_identifier(), {VrInput::kThumbstick});
  if (angular_controller_type_ != linear_controller_type_) {
    VrSession::instance().claim_inputs(
      angular_controller_type_, get_identifier(), {VrInput::kThumbstick});
  }
}

nlohmann::json VrBaseComponent::get_info() const {
  return nlohmann::json{
    {"type",                    get_type()},
    {"identifier",              get_identifier()},
    {"linear_controller_type",  linear_controller_type_},
    {"angular_controller_type", angular_controller_type_},
    {"vr_port",                 vr_port_},
    {"max_linear_mps",          max_linear_mps_},
    {"max_angular_rps",         max_angular_rps_},
    {"deadzone",                deadzone_},
    {"connected",               VrSession::instance().is_vr_connected()},
  };
}

void VrBaseComponent::prepare_for_teleop() {
  if (!VrSession::instance().wait_for_connection(connection_timeout_)) {
    throw std::runtime_error(
      "VrBaseComponent: timed out waiting for VR headset to connect "
      "on port " + std::to_string(vr_port_) +
      " — is the VR app running?");
  }
}

void VrBaseComponent::end_teleop() {
  if (session_held_) {
    VrSession::instance().release_claims(get_identifier());
    VrSession::instance().release();
    session_held_ = false;
  }
}

std::vector<float> VrBaseComponent::read() {
  // Hold zero velocity while the headset is disconnected so the base does not
  // coast on a stale frame.
  if (!VrSession::instance().is_vr_connected()) return {0.0f, 0.0f};

  const auto frame = VrSession::instance().latest_frame();
  if (!frame) return {0.0f, 0.0f};

  const auto& linear_stick = (linear_controller_type_ == "right")
    ? frame->right_controller.thumbstick
    : frame->left_controller.thumbstick;
  const auto& angular_stick = (angular_controller_type_ == "right")
    ? frame->right_controller.thumbstick
    : frame->left_controller.thumbstick;

  const double y = apply_deadzone(linear_stick.y_axis, deadzone_);
  const double x = apply_deadzone(angular_stick.x_axis, deadzone_);

  // Forward stick → forward velocity; left stick → positive yaw (CCW
  // about the vertical axis), so we negate x. `max_*` caps are applied
  // after the deadzone rescale so full-deflection matches the
  // user-configured limit.
  const double linear  =  y * max_linear_mps_;
  const double angular = -x * max_angular_rps_;
  return {static_cast<float>(linear), static_cast<float>(angular)};
}

void VrBaseComponent::write(const std::vector<float>& /*cmd*/) {
  // Leader only.
}

REGISTER_HARDWARE(VrBaseComponent, "vr_base_component")

}  // namespace trossen::hw::vr
