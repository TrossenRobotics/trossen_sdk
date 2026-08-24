/**
 * @file glide_arm_input_component.hpp
 * @brief Publishes Glide handle joystick/button state from the arm driver.
 */

#ifndef TROSSEN_SDK__HW__GLIDE__GLIDE_ARM_INPUT_COMPONENT_HPP_
#define TROSSEN_SDK__HW__GLIDE__GLIDE_ARM_INPUT_COMPONENT_HPP_

#include <string>
#include <vector>

#include "trossen_sdk/hw/glide/glide_session.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"

namespace trossen::hw::glide {

/**
 * @brief Fills the GlideSession reader seam from live arm drivers.
 *
 * A Glide handle's joystick and buttons arrive over the same libtrossen_arm
 * driver that streams its joint positions, reported alongside joint state in the
 * driver's own robot output. This component is the one place that knows that:
 * for each handle it names, it resolves the already-constructed
 * `TrossenArmComponent`, borrows its driver, and registers a `GlideInputReader`
 * that converts the driver's input report into an SDK-owned
 * `GlideInputSnapshot`. Every mapping component downstream reads snapshots and
 * never touches the driver.
 *
 * That containment is the point of keeping it a separate component rather than a
 * few lines inside `TrossenArmComponent`: the arm component has no reason to
 * know a handle exists, and the mappers have no reason to know a driver does.
 *
 * It claims nothing. Claims are made by the components that *consume* an input
 * — the base takes a joystick, session control takes buttons — never by the one
 * that publishes it, so a handle whose joystick nothing reads is simply unread.
 *
 * Ordering: the handles' arm components must already be registered in
 * `ActiveHardwareRegistry` when this one configures, which holds because
 * `hardware.arms` is constructed before `hardware.controls`. Nothing
 * enforces that, so configure() names it in the error when a lookup misses.
 *
 * Config as configure() receives it:
 * @code
 * { "arms": ["glide_left", "glide_right"] }
 * @endcode
 */
class GlideArmInputComponent : public HardwareComponent {
public:
  explicit GlideArmInputComponent(std::string identifier)
    : HardwareComponent(identifier) {}

  /// Unregisters every reader this component installed, so a stale reader can
  /// never outlive the driver it reads from.
  ~GlideArmInputComponent() override;

  /**
   * @brief Resolve each named handle and register its input reader.
   *
   * @throws std::runtime_error if `arms` is missing, empty, or holds anything
   *         but non-empty strings; or if a named id is not a registered
   *         `TrossenArmComponent` or has no driver.
   */
  void configure(const nlohmann::json& config) override;

  std::string get_type() const override { return "glide_arm_input"; }

  /// Reports the handle ids this component publishes readers for, in config
  /// order, under "arms".
  nlohmann::json get_info() const override;

private:
  /// Drop every registered reader. Idempotent; safe before configure().
  void unregister_all();

  std::vector<std::string> arm_ids_;
};

}  // namespace trossen::hw::glide

#endif  // TROSSEN_SDK__HW__GLIDE__GLIDE_ARM_INPUT_COMPONENT_HPP_
