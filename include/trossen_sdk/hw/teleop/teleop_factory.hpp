/**
 * @file teleop_factory.hpp
 * @brief Factory for constructing TeleopControllers from the global config.
 *
 * Reads the teleop block from `GlobalConfig::instance()`, resolves the
 * leader and follower referenced in each pair via `ActiveHardwareRegistry`,
 * and returns one ready-to-run `TeleopController` per pair.
 *
 * Prerequisite: the global config must already carry the teleop section,
 * and the hardware components referenced in each pair must be present in
 * `ActiveHardwareRegistry`.
 */

#ifndef TROSSEN_SDK__HW__TELEOP__TELEOP_FACTORY_HPP_
#define TROSSEN_SDK__HW__TELEOP__TELEOP_FACTORY_HPP_

#include <memory>
#include <vector>

#include "trossen_sdk/hw/teleop/teleop_controller.hpp"

namespace trossen::hw::teleop {

/**
 * @brief Construct one TeleopController per pair declared in the global config.
 *
 * Behavior:
 * - If teleop is disabled (`enabled = false`), returns an empty vector.
 * - For each pair: looks up `leader` and `follower` IDs in
 *   `ActiveHardwareRegistry`. If the leader is missing, prints a warning
 *   and skips that pair. If the follower is missing or absent, the
 *   controller is constructed in leader-only mode.
 * - If a referenced component does not implement `TeleopCapable`, prints
 *   a warning and skips that pair.
 * - Each returned controller is constructed but not yet running; the
 *   caller drives its lifecycle via `prepare_teleop()`, `teleop()`,
 *   `reset_teleop()`, and `stop_teleop()`.
 *
 * Throws `std::runtime_error` if the global "teleop" key is missing.
 */
std::vector<std::unique_ptr<TeleopController>>
create_controllers_from_global_config();

}  // namespace trossen::hw::teleop

#endif  // TROSSEN_SDK__HW__TELEOP__TELEOP_FACTORY_HPP_
