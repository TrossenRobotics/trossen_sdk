/**
 * @file glide_haptics.hpp
 * @brief Maps a follower's contact force onto a Glide handle's vibration.
 */

#ifndef TROSSEN_SDK__HW__GLIDE__GLIDE_HAPTICS_HPP_
#define TROSSEN_SDK__HW__GLIDE__GLIDE_HAPTICS_HPP_

#include <cstdint>
#include <string>

namespace trossen::hw::glide {

/**
 * @brief The force-to-intensity curve for one handle's vibration motor.
 *
 * Deliberately a pure value type with no hardware in sight: the mapping is the
 * part that needs tuning against an operator's hand, so it has to be unit
 * testable without an arm, a handle, or a driver. The component that owns a
 * handle reads a force, asks this for an intensity, and hands the result to
 * GlideSession.
 *
 * Four properties of the hardware shape the curve, and all four are why a plain
 * linear map from Newtons to 0-255 feels wrong:
 *
 *  - **A dead zone is mandatory, not polish.** The force this renders is the
 *    driver's `external_efforts` residual — what the model cannot explain by
 *    gravity or the commanded motion. A free-moving arm does not read zero:
 *    payload model error and inertia during fast motion both land there. Below
 *    `deadband_n` the handle stays silent, so the buzz means contact rather
 *    than "the arm is moving".
 *
 *  - **The motor has a start-up threshold.** An eccentric-rotating-mass motor
 *    does not spin at low duty; below roughly 60-80/255 it sits still and the
 *    operator feels nothing. So the first Newton past the dead zone jumps
 *    straight to `intensity_floor` rather than ramping up from 0, which would
 *    otherwise waste the bottom third of the force range on silence.
 *
 *  - **Perceived strength is not duty cycle.** `gamma` shapes the curve
 *    between the two ends: 1.0 is linear in duty, above 1.0 holds the buzz
 *    faint until the push is firm (a late onset), below 1.0 makes light
 *    contact obvious at the cost of headroom higher up.
 *
 *  - **Every distinct value costs a packet.** Intensity shares one
 *    `InputCommand` write with the button LEDs, so a continuously varying force
 *    would push a new packet every control tick. `levels` quantises the curve
 *    into that many steps, which is what lets GlideSession's change-detection
 *    swallow the overwhelming majority of writes: a hand leaning steadily on a
 *    table produces one packet, not thousands.
 */
struct GlideHapticCurve {
  /// Contact force at or below which the handle stays silent (N).
  float deadband_n{5.0f};

  /// Contact force that saturates the motor at `intensity_max` (N). Must be
  /// strictly greater than `deadband_n`.
  float max_n{40.0f};

  /// Duty applied the instant force passes `deadband_n` — the motor's start-up
  /// threshold, below which it does not spin at all.
  std::uint8_t intensity_floor{80};

  /// Duty at and above `max_n`. Must be >= `intensity_floor`.
  std::uint8_t intensity_max{255};

  /// Shaping exponent applied to the normalised force. Must be > 0.
  float gamma{1.0f};

  /// Number of distinct intensity steps between floor and max. Fewer steps mean
  /// fewer packets on the wire. Values below 2 disable quantisation entirely,
  /// which is useful for characterising the motor but wasteful in a poll loop.
  std::uint8_t levels{16};

  /**
   * @brief Intensity for `force_n`, in the driver's 0-255 duty units.
   *
   * Total: any force at or below the dead zone (and any non-finite force, so a
   * NaN can never latch the motor on) maps to a hard 0.
   */
  std::uint8_t intensity_for_force(float force_n) const;

  /**
   * @brief Reject a curve that cannot render anything sensible.
   *
   * Called at configure() time so a typo is a startup error naming the field,
   * rather than a handle that mysteriously never buzzes.
   *
   * @param who Identifier to name in the message (the owning component's id).
   * @throws std::invalid_argument on a non-positive span, an inverted intensity
   *         range, a non-positive gamma, or a negative dead zone.
   */
  void validate(const std::string& who) const;
};

}  // namespace trossen::hw::glide

#endif  // TROSSEN_SDK__HW__GLIDE__GLIDE_HAPTICS_HPP_
