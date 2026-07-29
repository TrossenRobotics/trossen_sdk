/**
 * @file glide_base_component.hpp
 * @brief Mobile-base teleop leader driven by Glide handle joysticks and buttons.
 */

#ifndef TROSSEN_SDK__HW__GLIDE__GLIDE_BASE_COMPONENT_HPP_
#define TROSSEN_SDK__HW__GLIDE__GLIDE_BASE_COMPONENT_HPP_

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

#include "trossen_sdk/hw/glide/glide_session.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"

namespace trossen::hw::glide {

/**
 * @brief Base-space teleop leader that reads Glide handle inputs.
 *
 * A separate component from the arms on purpose. The operator's hands are on
 * two Glide handles that each carry a joystick and buttons alongside the arm
 * itself, and those inputs drive a different follower (the mobile base) than
 * the arm joints do (the follower arms). Modelling them as one bundled device
 * would force a single teleop pair to carry both, so instead each role is its
 * own pair over the same physical handles, and `GlideSession` guarantees no two
 * components read the same input.
 *
 * Leader-only: `read()` samples the configured inputs, `write()` is a no-op.
 *
 * ### Axis mapping is configuration, not code
 *
 * Which stick axis drives which base axis, and which button bits raise and
 * lower the lift, differ per handle revision and per operator preference — and
 * the button bit assignments are not documented anywhere the SDK can see. So
 * the whole mapping is declarative: discovering that "raise" is really bit 4 is
 * a config edit, not a rebuild.
 *
 * Expected JSON:
 * @code
 * {
 *   "axes": {
 *     "linear":  { "arm_id": "glide_left",  "source": "joystick_x",
 *                  "invert": false, "max": 1.0, "deadzone": 0.1 },
 *     "angular": { "arm_id": "glide_right", "source": "joystick_x",
 *                  "invert": true,  "max": 1.0, "deadzone": 0.1 },
 *     "lateral": { "arm_id": "glide_left",  "source": "joystick_y",
 *                  "invert": true,  "max": 1.0, "deadzone": 0.1 },
 *     "lift":    { "arm_id": "glide_right", "source": "buttons",
 *                  "up_bit": 0, "down_bit": 2, "max": 8000.0 }
 *   }
 * }
 * @endcode
 *
 * Every axis is optional; an omitted axis reads 0. `max` is the value at full
 * deflection (units depend on the axis — m/s, rad/s, or actuator units/s), and
 * `deadzone` is applied to the *scaled* value so it is expressed in those same
 * units rather than raw counts.
 *
 * ### Holonomic bases: use `translation`, not two independent axes
 *
 * A swerve or omni base driven by one stick wants that stick treated as a single
 * 2D vector, which per-axis handling gets wrong in two ways:
 *
 *  - **Square deadzone.** Independent per-axis deadzones make the dead region a
 *    square, so a diagonal nudge inside the corner reads as zero while the same
 *    magnitude along an axis moves. Direction changes the threshold.
 *  - **Diagonal overspeed.** Two axes each capped at `max` reach `max * sqrt(2)`
 *    together, so a full diagonal is ~41% faster than straight ahead.
 *
 * The optional `translation` block fixes both by deriving forward and lateral
 * from one stick with a *radial* deadzone and a magnitude clamp:
 *
 * @code
 * {
 *   "translation": {
 *     "arm_id": "glide_right",
 *     "forward_source": "joystick_y", "forward_invert": true,
 *     "lateral_source": "joystick_x", "lateral_invert": false,
 *     "max": 0.6, "deadzone": 0.05
 *   },
 *   "axes": {
 *     "angular": { "arm_id": "glide_left", "source": "joystick_x", "max": 1.2 },
 *     "lift":    { "arm_id": "glide_right", "source": "buttons",
 *                  "up_bit": 0, "down_bit": 2, "max": 8000.0 }
 *   }
 * }
 * @endcode
 *
 * `translation` and the `linear` / `lateral` entries in `axes` are mutually
 * exclusive — configuring both for the same axis is rejected rather than
 * silently letting one win.
 *
 * A differential-drive base has no lateral axis to pair, so it keeps using
 * independent `linear` and `angular` entries; nothing about the existing form
 * changes.
 */
class GlideBaseComponent : public HardwareComponent, public teleop::BaseSpaceTeleop {
public:
  explicit GlideBaseComponent(std::string identifier)
    : HardwareComponent(std::move(identifier)) {}

  /// Releases this component's input claims via the lease member.
  ~GlideBaseComponent() override = default;

  GlideBaseComponent(const GlideBaseComponent&)            = delete;
  GlideBaseComponent& operator=(const GlideBaseComponent&) = delete;
  GlideBaseComponent(GlideBaseComponent&&)                 = delete;
  GlideBaseComponent& operator=(GlideBaseComponent&&)      = delete;

  /**
   * @brief Parse the axis map and claim every input it references.
   *
   * @throws std::runtime_error if an input is already claimed by another
   *         component, or if two axes here map to the same input.
   * @throws std::invalid_argument on an unknown source name or a buttons axis
   *         without at least one of up_bit / down_bit.
   */
  void configure(const nlohmann::json& config) override;

  std::string get_type() const override { return "glide_base"; }

  nlohmann::json get_info() const override;

  /**
   * @brief Sample the configured inputs.
   *
   * @return Always `base_axis::kMaxSize` elements
   *         `[linear, angular, lift, lateral]`. Unconfigured axes and handles
   *         with no reader available read 0, which for a velocity command means
   *         "hold still" — the safe value when input is missing.
   */
  std::vector<float> read() override;

  /// No-op: the handles are an input device, not a base.
  void write(const std::vector<float>& cmd) override { (void)cmd; }

private:
  /// Where one base axis gets its value.
  struct AxisMap {
    enum class Source { kJoystickX, kJoystickY, kButtons };

    bool        configured{false};
    std::string arm_id;
    Source      source{Source::kJoystickX};

    /// Flip the sign after scaling — for a stick mounted so that "forward"
    /// reads as a decreasing count.
    bool  invert{false};

    /// Output value at full deflection, in the axis's own units.
    float max{1.0f};

    /// Ignore scaled magnitudes below this, so a stick that does not quite
    /// recentre does not creep the base.
    float deadzone{0.0f};

    /// Button bits for a `kButtons` axis. -1 means that direction is unmapped.
    int up_bit{-1};
    int down_bit{-1};
  };

  /// One stick treated as a 2D translation vector.
  struct TranslationMap {
    bool        configured{false};
    std::string arm_id;

    AxisMap::Source forward_source{AxisMap::Source::kJoystickY};
    AxisMap::Source lateral_source{AxisMap::Source::kJoystickX};
    bool            forward_invert{false};
    bool            lateral_invert{false};

    /// Magnitude at full deflection, in m/s. The clamp is on the *vector*
    /// magnitude, so a diagonal is no faster than a straight push.
    float max{1.0f};

    /// Radial dead region, in the same m/s units as `max`. Applied to the
    /// vector magnitude, making the dead zone a circle.
    float deadzone{0.0f};
  };

  /// Snapshots for this tick, one entry per distinct handle.
  ///
  /// Every axis used to read its handle independently, so a four-axis config
  /// hit the driver four times and could mix a left-handle reading from one
  /// driver cycle with a right-handle reading from the next. For a swerve base
  /// that skew shows up as rotation and translation disagreeing about when the
  /// operator moved. Reading each handle once per `read()` removes the skew and
  /// cuts the driver traffic.
  using SnapshotCache = std::unordered_map<std::string, std::optional<GlideInputSnapshot>>;

  /// Read every handle this component needs, once.
  SnapshotCache collect_snapshots() const;

  /// Raw stick axis normalised to -1..1 with inversion applied, before scaling.
  static float normalised_axis(const GlideInputSnapshot& snapshot,
                               AxisMap::Source source, bool invert);

  /// Evaluate one independent axis. Returns 0 for an unconfigured axis or an
  /// unavailable handle.
  float sample_axis(const AxisMap& axis, const SnapshotCache& cache) const;

  /// Evaluate the translation pair into `{forward, lateral}`.
  std::pair<float, float> sample_translation(const SnapshotCache& cache) const;

  /// Indexed by the `base_axis::k*` constants, so the read() vector is built by
  /// position with no separate ordering to keep in sync.
  std::array<AxisMap, teleop::base_axis::kMaxSize> axes_{};

  TranslationMap translation_{};

  GlideClaimLease lease_;
};

}  // namespace trossen::hw::glide

#endif  // TROSSEN_SDK__HW__GLIDE__GLIDE_BASE_COMPONENT_HPP_
