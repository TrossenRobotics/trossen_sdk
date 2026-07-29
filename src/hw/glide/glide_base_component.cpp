/**
 * @file glide_base_component.cpp
 * @brief Implementation of GlideBaseComponent.
 */

#include "trossen_sdk/hw/glide/glide_base_component.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

#include "trossen_sdk/hw/hardware_registry.hpp"

namespace trossen::hw::glide {

namespace ba = teleop::base_axis;

namespace {

/// Axis names as they appear in config, indexed by the base_axis constants so
/// the JSON key and the vector position cannot drift apart.
constexpr std::array<const char*, ba::kMaxSize> kAxisNames{
  "linear",   // ba::kLinear
  "angular",  // ba::kAngular
  "lift",     // ba::kLift
  "lateral",  // ba::kLateral
};

/**
 * @brief Map a raw joystick count onto a signed output range with a deadzone.
 *
 * The handle reports 0..4095 with centre at the midpoint, so a resting stick is
 * ~2047 and must be recentred before it means anything. The deadzone is applied
 * to the scaled result, which keeps it expressed in output units (m/s, rad/s)
 * rather than raw counts — so retuning it does not require knowing the ADC
 * range.
 */
float scale_stick(std::uint16_t raw, float max, float deadzone) {
  constexpr float span = static_cast<float>(kJoystickMax - kJoystickMin);
  const float centred = (static_cast<float>(raw) - static_cast<float>(kJoystickMin)) / span;
  // 0..1 -> -1..1
  const float normalised = centred * 2.0f - 1.0f;
  const float scaled = normalised * max;
  if (deadzone > 0.0f && std::abs(scaled) < deadzone) return 0.0f;
  return scaled;
}

}  // namespace

void GlideBaseComponent::configure(const nlohmann::json& config) {
  if (!config.contains("axes") || !config.at("axes").is_object()) {
    throw std::invalid_argument(
      "GlideBaseComponent '" + get_identifier() +
      "': config requires an 'axes' object; with no axes mapped the component "
      "would report zero velocity forever");
  }
  const auto& axes_json = config.at("axes");

  // Parse fully before claiming anything: a claim failure partway through would
  // otherwise leave this component holding inputs it never got to use.
  std::array<AxisMap, ba::kMaxSize> parsed{};

  for (std::size_t i = 0; i < ba::kMaxSize; ++i) {
    const char* name = kAxisNames[i];
    if (!axes_json.contains(name)) continue;
    const auto& j = axes_json.at(name);

    AxisMap axis;
    axis.configured = true;

    if (!j.contains("arm_id")) {
      throw std::invalid_argument(
        std::string("GlideBaseComponent: axis '") + name + "' requires 'arm_id'");
    }
    axis.arm_id = j.at("arm_id").get<std::string>();

    const auto source = j.value("source", std::string{"joystick_x"});
    if (source == "joystick_x") {
      axis.source = AxisMap::Source::kJoystickX;
    } else if (source == "joystick_y") {
      axis.source = AxisMap::Source::kJoystickY;
    } else if (source == "buttons") {
      axis.source = AxisMap::Source::kButtons;
    } else {
      throw std::invalid_argument(
        std::string("GlideBaseComponent: axis '") + name + "' has unknown source '" +
        source + "' (expected joystick_x, joystick_y, or buttons)");
    }

    axis.invert   = j.value("invert", false);
    axis.max      = j.value("max", 1.0f);
    axis.deadzone = j.value("deadzone", 0.0f);
    axis.up_bit   = j.value("up_bit", -1);
    axis.down_bit = j.value("down_bit", -1);

    if (axis.source == AxisMap::Source::kButtons &&
        axis.up_bit < 0 && axis.down_bit < 0) {
      throw std::invalid_argument(
        std::string("GlideBaseComponent: axis '") + name +
        "' uses source 'buttons' but maps neither up_bit nor down_bit, so it "
        "could never produce a non-zero value");
    }

    if (!std::isfinite(axis.max) || axis.max <= 0.0f) {
      throw std::invalid_argument(
        std::string("GlideBaseComponent: axis '") + name +
        "' has max " + std::to_string(axis.max) + "; must be finite and positive");
    }

    parsed[i] = std::move(axis);
  }

  // Claim every referenced input. Overlap between two axes here surfaces as the
  // same conflict error as overlap with another component, since both are the
  // same mistake: one input driving two things.
  GlideClaimLease lease;
  for (const auto& axis : parsed) {
    if (!axis.configured) continue;
    switch (axis.source) {
      case AxisMap::Source::kJoystickX:
      case AxisMap::Source::kJoystickY:
        lease.add(axis.arm_id, get_identifier(), {GlideClaim{GlideInput::kJoystick}});
        break;
      case AxisMap::Source::kButtons:
        if (axis.up_bit >= 0) {
          lease.add(axis.arm_id, get_identifier(), {glide_button(axis.up_bit)});
        }
        if (axis.down_bit >= 0) {
          lease.add(axis.arm_id, get_identifier(), {glide_button(axis.down_bit)});
        }
        break;
    }
  }

  axes_  = std::move(parsed);
  lease_ = std::move(lease);
}

float GlideBaseComponent::sample_axis(const AxisMap& axis) const {
  if (!axis.configured) return 0.0f;

  const auto snapshot = GlideSession::instance().read_inputs(axis.arm_id);
  if (!snapshot) return 0.0f;

  float value = 0.0f;
  switch (axis.source) {
    case AxisMap::Source::kJoystickX:
      value = scale_stick(snapshot->joystick_x, axis.max, axis.deadzone);
      break;
    case AxisMap::Source::kJoystickY:
      value = scale_stick(snapshot->joystick_y, axis.max, axis.deadzone);
      break;
    case AxisMap::Source::kButtons: {
      // Momentary buttons give a three-state axis: full speed either way, or
      // stopped. Both held cancels rather than picking one, so a stuck button
      // cannot run the actuator into its end stop.
      const bool up   = axis.up_bit   >= 0 && snapshot->button(axis.up_bit);
      const bool down = axis.down_bit >= 0 && snapshot->button(axis.down_bit);
      value = axis.max * (static_cast<float>(up) - static_cast<float>(down));
      break;
    }
  }

  return axis.invert ? -value : value;
}

std::vector<float> GlideBaseComponent::read() {
  std::vector<float> out(ba::kMaxSize, 0.0f);
  for (std::size_t i = 0; i < ba::kMaxSize; ++i) {
    out[i] = sample_axis(axes_[i]);
  }
  return out;
}

nlohmann::json GlideBaseComponent::get_info() const {
  nlohmann::json axes = nlohmann::json::object();
  for (std::size_t i = 0; i < ba::kMaxSize; ++i) {
    const auto& axis = axes_[i];
    if (!axis.configured) continue;
    axes[kAxisNames[i]] = {
      {"arm_id", axis.arm_id},
      {"max", axis.max},
      {"deadzone", axis.deadzone},
      {"invert", axis.invert},
    };
  }
  return {
    {"type", get_type()},
    {"axes", axes},
  };
}

REGISTER_HARDWARE(GlideBaseComponent, "glide_base")

}  // namespace trossen::hw::glide
