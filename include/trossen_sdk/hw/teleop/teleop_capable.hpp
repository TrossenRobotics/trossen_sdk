/**
 * @file teleop_capable.hpp
 * @brief Mixin interface for hardware components that can participate in teleoperation.
 *
 * A hardware component that can act as a teleop leader or follower (i.e. an arm)
 * implements this interface in addition to inheriting from HardwareComponent.
 * Components that cannot teleoperate (cameras, mobile bases, sensors) simply do not
 * implement it, which keeps the HardwareComponent base interface free of capability-
 * specific vocabulary and lets TeleopController accept only valid inputs at compile
 * time instead of guarding at runtime.
 *
 * Example:
 * @code
 *   class TrossenArmComponent : public HardwareComponent, public TeleopCapable { ... };
 *
 *   auto hw = HardwareRegistry::create("trossen_arm", "leader", cfg);
 *   auto leader = trossen::hw::teleop::as_capable<TeleopCapable>(hw);
 *   if (!leader) {
 *     throw std::invalid_argument("'leader' is not teleop-capable");
 *   }
 *   TeleopController ctrl(leader, follower, {});
 * @endcode
 */

#ifndef TROSSEN_SDK__HW__TELEOP__TELEOP_CAPABLE_HPP_
#define TROSSEN_SDK__HW__TELEOP__TELEOP_CAPABLE_HPP_

#include <cstddef>
#include <memory>
#include <vector>

#include "trossen_sdk/hw/hardware_component.hpp"

namespace trossen::hw::teleop {

/**
 * @brief Mixin interface for hardware that supports teleoperation.
 *
 * Concrete teleop-capable components inherit from this in addition to
 * HardwareComponent. The interface is intentionally stateless — implementations
 * keep their state on the concrete class (typically the underlying driver).
 */
class TeleopCapable {
public:
  virtual ~TeleopCapable() = default;

  /**
   * @brief Number of controllable joints exposed by this component.
   */
  virtual std::size_t num_joints() const = 0;

  /**
   * @brief Read the current joint positions.
   *
   * Used by TeleopController to read the leader's state each control tick.
   */
  virtual std::vector<float> get_joint_positions() = 0;

  /**
   * @brief Command joint positions on this component.
   *
   * Used by TeleopController to drive the follower each control tick.
   */
  virtual void set_joint_positions(const std::vector<float>& positions) = 0;

  /**
   * @brief Prepare this component to act as a teleop leader.
   *
   * Called once when the teleop session starts. For arms, this typically
   * enables gravity compensation (external_effort mode).
   */
  virtual void prepare_for_leader() = 0;

  /**
   * @brief Prepare this component to act as a teleop follower.
   *
   * Called once when the teleop session starts. For arms, this typically
   * sets position mode and moves to match the leader's initial positions.
   *
   * @param initial_positions Leader's current positions to match.
   */
  virtual void prepare_for_follower(const std::vector<float>& initial_positions) = 0;

  /**
   * @brief Clean up after a teleop session ends.
   *
   * Called when the teleop session stops. For arms, this typically returns the
   * hardware to a safe idle state.
   */
  virtual void cleanup_teleop() = 0;
};

/**
 * @brief Convenience helper to query a HardwareComponent for a capability.
 *
 * Returns a shared_ptr to the requested capability interface if the component
 * implements it, or nullptr otherwise. Wraps std::dynamic_pointer_cast so call
 * sites read as a domain-level capability check rather than a raw RTTI cast.
 *
 * @tparam Capability The capability interface to query (e.g. TeleopCapable).
 * @param hw Hardware component to inspect.
 * @return Shared pointer to the capability, or nullptr if not implemented.
 */
template <typename Capability>
std::shared_ptr<Capability> as_capable(const std::shared_ptr<HardwareComponent>& hw) {
  return std::dynamic_pointer_cast<Capability>(hw);
}

}  // namespace trossen::hw::teleop

#endif  // TROSSEN_SDK__HW__TELEOP__TELEOP_CAPABLE_HPP_
