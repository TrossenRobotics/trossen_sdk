/**
 * @file glide_base_component.hpp
 * @brief Mobile-base teleop leader driven by Glide handle joysticks and buttons.
 */

#ifndef TROSSEN_SDK__HW__GLIDE__GLIDE_BASE_COMPONENT_HPP_
#define TROSSEN_SDK__HW__GLIDE__GLIDE_BASE_COMPONENT_HPP_

#include <array>
#include <string>
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

  /// Evaluate one axis against a handle snapshot. Returns 0 for an
  /// unconfigured axis or an unavailable handle.
  float sample_axis(const AxisMap& axis) const;

  /// Indexed by the `base_axis::k*` constants, so the read() vector is built by
  /// position with no separate ordering to keep in sync.
  std::array<AxisMap, teleop::base_axis::kMaxSize> axes_{};

  GlideClaimLease lease_;
};

}  // namespace trossen::hw::glide

#endif  // TROSSEN_SDK__HW__GLIDE__GLIDE_BASE_COMPONENT_HPP_
