/**
 * @file teleop_factory.hpp
 * @brief Factory for constructing TeleopControllers from the global config.
 *
 * Mirrors the SessionManager pattern: read teleop wiring from
 * `GlobalConfig::instance()`, resolve hardware references via
 * `ActiveHardwareRegistry`, and return ready-to-start `TeleopController`
 * instances.
 *
 * Prerequisite: `SdkConfig::populate_global_config()` must have been called,
 * and the hardware components referenced in the teleop pairs must already be
 * registered in `ActiveHardwareRegistry` (typically by `HardwareRegistry::create`
 * with `mark_active=true`).
 *
 * Usage:
 * @code
 *   auto cfg = trossen::configuration::SdkConfig::from_json(j);
 *   cfg.populate_global_config();
 *   // ... create hardware via HardwareRegistry::create(..., mark_active=true) ...
 *
 *   auto controllers = trossen::hw::teleop::create_controllers_from_global_config();
 *   for (auto& c : controllers) c->start();
 * @endcode
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
 *   `ActiveHardwareRegistry`. If the leader is missing, prints a warning and
 *   skips that pair. If the follower is missing or absent, the controller is
 *   constructed in leader-only mode (UMI-style).
 * - If a referenced component does not implement `TeleopCapable` (e.g. a
 *   camera was named by mistake), prints a warning and skips that pair.
 * - Each returned controller is constructed but **not yet started**; the
 *   caller is responsible for invoking `start()` and `stop()`.
 *
 * Throws `std::runtime_error` if the global "teleop" key is missing
 * (i.e. `populate_global_config()` was never called).
 */
std::vector<std::unique_ptr<TeleopController>>
create_controllers_from_global_config();

}  // namespace trossen::hw::teleop

#endif  // TROSSEN_SDK__HW__TELEOP__TELEOP_FACTORY_HPP_
