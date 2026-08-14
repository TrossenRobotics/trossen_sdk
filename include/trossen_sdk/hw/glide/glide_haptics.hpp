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

/**
 * @brief Tracks the resting force level so only the deviation from it is felt.
 *
 * Separated from the hardware for the same reason as GlideHapticCurve: this is
 * the part whose behaviour over time decides whether the handle is quiet, and
 * that has to be testable without an arm.
 *
 * It exists because the signal the haptic channel is given is NOT a contact
 * force. It is the driver's external-effort residual, which carries whatever
 * gravity and payload the controller's model cannot account for. Measured on an
 * idle Rivet follower with nothing touching it, that reads ~50 N — ten times the
 * default dead zone, enough to hold the motor at full duty forever.
 *
 * A configured offset cannot fix it, because the offset is not constant: it
 * moves with the arm's pose, and it is far larger on a limp arm than on the same
 * arm actively holding position. So the resting level is re-measured
 * continuously and contact is rendered as a DEVIATION from it — which matches
 * what contact physically is, a change.
 *
 * The one subtlety is that the tracker must not learn the thing it is trying to
 * detect. Adaptation is FROZEN while the deviation exceeds the dead zone, so a
 * sustained push cannot be absorbed into the baseline and fade out under the
 * operator's hand. Smaller deviations — including negative ones, when a contact
 * is released — always adapt, so an arm that started already in contact
 * recovers by itself rather than staying numb for the session.
 */
class HapticBaseline {
public:
  HapticBaseline() = default;

  /// @param tau_s Seconds over which the resting level is tracked. Zero means
  ///        never adapt, which renders the raw residual — useful only for
  ///        characterising an arm.
  /// @param deadband_n Deviation above which adaptation freezes. Pass the same
  ///        dead zone the curve uses, or the tracker will keep learning through
  ///        exactly the contacts the curve renders.
  HapticBaseline(float tau_s, float deadband_n)
    : tau_s_(tau_s), deadband_n_(deadband_n) {}

  /**
   * @brief Feed one raw residual reading and get back what should be rendered.
   *
   * @param raw_n The driver's residual magnitude (N).
   * @param now_s Monotonic seconds. Adaptation uses the real elapsed time, so
   *        the time constant means the same thing at any loop rate.
   * @return Deviation above the resting level in N, never negative. Zero on the
   *         first call of a session (that call establishes the level) and on a
   *         non-finite input.
   */
  float deviation_for(float raw_n, double now_s);

  /// Forget the resting level, so the next reading measures it afresh. Called on
  /// teardown: a level learned with the arm limp, or in another pose, would
  /// otherwise be carried into a session starting somewhere else.
  void reset();

  /// Whether a resting level has been measured yet. Diagnostics and tests.
  bool measured() const { return measured_; }

  /// The tracked resting level (N). Meaningless until measured(). Diagnostics.
  float level() const { return static_cast<float>(baseline_n_); }

private:
  float tau_s_{3.0f};
  float deadband_n_{5.0f};

  /// Held as double because it is an accumulator: a long session applies many
  /// thousands of small exponential increments to it.
  double baseline_n_{0.0};
  double last_s_{0.0};
  bool   measured_{false};
};

}  // namespace trossen::hw::glide

#endif  // TROSSEN_SDK__HW__GLIDE__GLIDE_HAPTICS_HPP_
