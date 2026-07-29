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
 * @brief Recentre a raw joystick count to -1..1.
 *
 * The handle reports 0..4095 with centre at the midpoint, so a resting stick is
 * ~2047 and means nothing until recentred.
 */
float normalise_stick(std::uint16_t raw) {
  constexpr float span = static_cast<float>(kJoystickMax - kJoystickMin);
  const float centred = (static_cast<float>(raw) - static_cast<float>(kJoystickMin)) / span;
  return centred * 2.0f - 1.0f;
}

/**
 * @brief Scale a single normalised axis, with the deadzone in output units.
 *
 * For a one-dimensional axis (yaw from a stick pushed sideways) a scalar
 * deadzone is the right model. Two-dimensional translation must not use this —
 * see `sample_translation()` for why.
 */
float scale_axis(float normalised, float max, float deadzone) {
  const float scaled = normalised * max;
  if (deadzone > 0.0f && std::abs(scaled) < deadzone) return 0.0f;
  return scaled;
}

}  // namespace

void GlideBaseComponent::configure(const nlohmann::json& config) {
  const bool has_axes = config.contains("axes") && config.at("axes").is_object();
  const bool has_translation =
    config.contains("translation") && config.at("translation").is_object();

  if (!has_axes && !has_translation) {
    throw std::invalid_argument(
      "GlideBaseComponent '" + get_identifier() +
      "': config requires an 'axes' object, a 'translation' object, or both; "
      "with nothing mapped the component would report zero velocity forever");
  }

  TranslationMap translation;
  if (has_translation) {
    const auto& j = config.at("translation");
    translation.configured = true;

    if (!j.contains("arm_id")) {
      throw std::invalid_argument(
        "GlideBaseComponent: 'translation' requires 'arm_id'");
    }
    translation.arm_id = j.at("arm_id").get<std::string>();

    auto parse_source = [](const std::string& name, const char* field) {
      if (name == "joystick_x") return AxisMap::Source::kJoystickX;
      if (name == "joystick_y") return AxisMap::Source::kJoystickY;
      throw std::invalid_argument(
        std::string("GlideBaseComponent: translation '") + field +
        "' must be joystick_x or joystick_y (got '" + name +
        "'); a translation vector cannot come from buttons");
    };
    translation.forward_source =
      parse_source(j.value("forward_source", std::string{"joystick_y"}), "forward_source");
    translation.lateral_source =
      parse_source(j.value("lateral_source", std::string{"joystick_x"}), "lateral_source");

    if (translation.forward_source == translation.lateral_source) {
      throw std::invalid_argument(
        "GlideBaseComponent: translation forward_source and lateral_source are "
        "both the same stick axis, which would make the base only ever drive "
        "diagonally");
    }

    translation.forward_invert = j.value("forward_invert", false);
    translation.lateral_invert = j.value("lateral_invert", false);
    translation.max            = j.value("max", 1.0f);
    translation.deadzone       = j.value("deadzone", 0.0f);

    if (!std::isfinite(translation.max) || translation.max <= 0.0f) {
      throw std::invalid_argument(
        "GlideBaseComponent: translation 'max' must be finite and positive");
    }
    if (translation.deadzone < 0.0f || translation.deadzone >= translation.max) {
      throw std::invalid_argument(
        "GlideBaseComponent: translation 'deadzone' must be in [0, max); a "
        "deadzone at or above max would zero every command");
    }
  }

  // Bind to a real lvalue in both branches rather than a ternary temporary, so
  // there is no lifetime subtlety and no copy of the axes object.
  static const nlohmann::json kNoAxes = nlohmann::json::object();
  const nlohmann::json& axes_json = has_axes ? config.at("axes") : kNoAxes;

  if (has_translation) {
    for (const char* owned : {"linear", "lateral"}) {
      if (axes_json.contains(owned)) {
        throw std::invalid_argument(
          std::string("GlideBaseComponent '") + get_identifier() +
          "': axis '" + owned + "' is set in 'axes' while 'translation' is also "
          "configured. Translation owns both linear and lateral; remove one so "
          "it is unambiguous which produces the command.");
      }
    }
  }

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
  if (translation.configured) {
    lease.add(translation.arm_id, get_identifier(),
              {GlideClaim{GlideInput::kJoystick}});
  }
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

  axes_        = std::move(parsed);
  translation_ = std::move(translation);
  lease_       = std::move(lease);
}

float GlideBaseComponent::normalised_axis(const GlideInputSnapshot& snapshot,
                                          AxisMap::Source source, bool invert) {
  float value = 0.0f;
  switch (source) {
    case AxisMap::Source::kJoystickX:
      value = normalise_stick(snapshot.joystick_x);
      break;
    case AxisMap::Source::kJoystickY:
      value = normalise_stick(snapshot.joystick_y);
      break;
    case AxisMap::Source::kButtons:
      // Not meaningful as a continuous axis; callers handle buttons separately.
      value = 0.0f;
      break;
  }
  return invert ? -value : value;
}

GlideBaseComponent::SnapshotCache GlideBaseComponent::collect_snapshots() const {
  SnapshotCache cache;

  auto want = [&cache](const std::string& arm_id) {
    if (arm_id.empty()) return;
    // emplace leaves an existing entry alone, so each handle is read once even
    // when several axes name it.
    if (cache.find(arm_id) == cache.end()) {
      cache.emplace(arm_id, GlideSession::instance().read_inputs(arm_id));
    }
  };

  if (translation_.configured) want(translation_.arm_id);
  for (const auto& axis : axes_) {
    if (axis.configured) want(axis.arm_id);
  }
  return cache;
}

float GlideBaseComponent::sample_axis(const AxisMap& axis,
                                      const SnapshotCache& cache) const {
  if (!axis.configured) return 0.0f;

  const auto it = cache.find(axis.arm_id);
  if (it == cache.end() || !it->second) return 0.0f;
  const GlideInputSnapshot& snapshot = *it->second;

  if (axis.source == AxisMap::Source::kButtons) {
    // Momentary buttons give a three-state axis: full speed either way, or
    // stopped. Both held cancels rather than picking one, so a stuck button
    // cannot run the actuator into its end stop.
    const bool up   = axis.up_bit   >= 0 && snapshot.button(axis.up_bit);
    const bool down = axis.down_bit >= 0 && snapshot.button(axis.down_bit);
    const float value = axis.max * (static_cast<float>(up) - static_cast<float>(down));
    return axis.invert ? -value : value;
  }

  return scale_axis(normalised_axis(snapshot, axis.source, axis.invert),
                    axis.max, axis.deadzone);
}

std::pair<float, float> GlideBaseComponent::sample_translation(
  const SnapshotCache& cache) const {
  if (!translation_.configured) return {0.0f, 0.0f};

  const auto it = cache.find(translation_.arm_id);
  if (it == cache.end() || !it->second) return {0.0f, 0.0f};
  const GlideInputSnapshot& snapshot = *it->second;

  const float fwd = normalised_axis(snapshot, translation_.forward_source,
                                    translation_.forward_invert);
  const float lat = normalised_axis(snapshot, translation_.lateral_source,
                                    translation_.lateral_invert);

  // Treat the stick as a vector, not two numbers. Two things fall out of this
  // that per-axis handling gets wrong on a holonomic base:
  //
  //  - The dead region is a circle, so the threshold to start moving is the
  //    same in every direction. Per-axis deadzones make it a square, where a
  //    diagonal nudge inside the corner reads as zero.
  //  - Magnitude is clamped to `max`, so a full diagonal is exactly as fast as
  //    a full push forward. Independent axes would reach max*sqrt(2) together,
  //    making diagonals ~41% faster — a real surprise on a swerve base.
  const float magnitude = std::hypot(fwd, lat);
  if (magnitude <= 0.0f) return {0.0f, 0.0f};

  // Deadzone is configured in output units (m/s) for consistency with the
  // scalar axes, so convert to the normalised domain the magnitude lives in.
  const float dead_norm = (translation_.max > 0.0f)
    ? translation_.deadzone / translation_.max
    : 0.0f;
  if (magnitude <= dead_norm) return {0.0f, 0.0f};

  // Rescale the surviving range back onto 0..1 so output is continuous from
  // zero at the deadzone edge rather than jumping to the deadzone value.
  const float usable = 1.0f - dead_norm;
  float scaled = usable > 0.0f ? (magnitude - dead_norm) / usable : 0.0f;
  scaled = std::min(scaled, 1.0f);

  // Preserve direction: divide by the original magnitude to get the unit
  // vector, then apply the clamped speed.
  const float speed = scaled * translation_.max;
  return {fwd / magnitude * speed, lat / magnitude * speed};
}

std::vector<float> GlideBaseComponent::read() {
  // One read per handle for the whole tick, so rotation and translation cannot
  // come from different driver cycles.
  const SnapshotCache cache = collect_snapshots();

  std::vector<float> out(ba::kMaxSize, 0.0f);
  for (std::size_t i = 0; i < ba::kMaxSize; ++i) {
    out[i] = sample_axis(axes_[i], cache);
  }

  if (translation_.configured) {
    const auto [forward, lateral] = sample_translation(cache);
    out[ba::kLinear]  = forward;
    out[ba::kLateral] = lateral;
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
  nlohmann::json info = {
    {"type", get_type()},
    {"axes", axes},
  };
  if (translation_.configured) {
    info["translation"] = {
      {"arm_id", translation_.arm_id},
      {"max", translation_.max},
      {"deadzone", translation_.deadzone},
      {"radial", true},
    };
  }
  return info;
}

REGISTER_HARDWARE(GlideBaseComponent, "glide_base")

}  // namespace trossen::hw::glide
