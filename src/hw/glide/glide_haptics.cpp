#include "trossen_sdk/hw/glide/glide_haptics.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace trossen::hw::glide {

std::uint8_t GlideHapticCurve::intensity_for_force(float force_n) const {
  // A non-finite force means the arm's own state is garbage, and the motor
  // latches until told otherwise — so this case has to be silence, not an
  // undefined cast. Reached in practice when a driver read races a reconnect.
  if (!std::isfinite(force_n)) return 0;

  // Callers pass a magnitude, but taking it again costs nothing and means a
  // sign convention change upstream cannot silently invert the whole curve.
  const float force = std::fabs(force_n);
  if (force <= deadband_n) return 0;

  const float span = max_n - deadband_n;  // validate() guarantees span > 0
  float norm = (force - deadband_n) / span;
  norm = std::clamp(norm, 0.0f, 1.0f);

  // Shape before quantising, so the steps are evenly spaced in what the
  // operator feels rather than in Newtons.
  float shaped = (gamma == 1.0f) ? norm : std::pow(norm, gamma);

  if (levels >= 2) {
    const float steps = static_cast<float>(levels - 1);
    shaped = std::round(shaped * steps) / steps;
  }

  const float floor_f = static_cast<float>(intensity_floor);
  const float max_f   = static_cast<float>(intensity_max);
  // The floor is the value at shaped == 0: the first Newton past the dead zone
  // must already spin the motor, or the bottom of the range is inaudible.
  const float duty = floor_f + shaped * (max_f - floor_f);

  return static_cast<std::uint8_t>(
    std::clamp(std::round(duty), floor_f, max_f));
}

void GlideHapticCurve::validate(const std::string& who) const {
  if (deadband_n < 0.0f) {
    throw std::invalid_argument(
      who + ": haptic_force_deadband_n must be >= 0, got " +
      std::to_string(deadband_n));
  }
  if (!(max_n > deadband_n)) {
    throw std::invalid_argument(
      who + ": haptic_force_max_n (" + std::to_string(max_n) +
      ") must be greater than haptic_force_deadband_n (" +
      std::to_string(deadband_n) + ")");
  }
  if (intensity_max < intensity_floor) {
    throw std::invalid_argument(
      who + ": haptic_intensity_max (" + std::to_string(intensity_max) +
      ") must be >= haptic_intensity_floor (" +
      std::to_string(intensity_floor) + ")");
  }
  if (!(gamma > 0.0f)) {
    throw std::invalid_argument(
      who + ": haptic_curve_gamma must be > 0, got " + std::to_string(gamma));
  }
}

}  // namespace trossen::hw::glide
