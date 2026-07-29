/**
 * @file component_config.hpp
 * @brief Generic, registry-resolved hardware component declaration.
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__COMPONENT_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__COMPONENT_CONFIG_HPP_

#include <stdexcept>
#include <string>

#include "nlohmann/json.hpp"

namespace trossen::configuration {

/**
 * @brief One hardware component named by its registry type.
 *
 * The typed maps in HardwareConfig — `arms`, `cameras` — each exist because
 * something in the SDK reads their fields directly. Most components need no such
 * treatment: they are constructed by id and type through
 * `HardwareRegistry::create()` and parse their own JSON in `configure()`. Adding
 * a bespoke config struct and a `HardwareConfig` map per component type would
 * mean touching the shared config schema for every new piece of hardware, which
 * is how the first Rivet implementation ended up adding `bimanual_arms` and
 * `mobile_rivet` as top-level maps for one robot.
 *
 * So this carries only what dispatch needs — an id and a type — and hands the
 * rest through untouched. A new `REGISTER_HARDWARE` type becomes usable from
 * config with no schema change at all, which is what lets the Glide input layer
 * and the Rivet base be declared by both the standalone example and the webapp
 * without either learning their field names.
 *
 * Expected JSON (a list under `hardware.components`):
 * @code
 * [
 *   { "id": "base_leader", "type": "glide_base",
 *     "translation": { "arm_id": "glide_left", ... },
 *     "axes": { "angular": { ... }, "lift": { ... } } },
 *   { "id": "session_control", "type": "glide_session_control",
 *     "buttons": [ ... ] },
 *   { "id": "rivet_base", "type": "trossen_base" }
 * ]
 * @endcode
 */
struct ComponentConfig {
  /// @brief Logical id, unique across every hardware map. Used as the teleop
  /// pair endpoint name and the ActiveHardwareRegistry key.
  std::string id;

  /// @brief Registry type, e.g. "glide_base". Must be a REGISTER_HARDWARE name.
  std::string type;

  /// @brief The entry verbatim, forwarded to the component's configure().
  ///
  /// Kept whole rather than stripped of `id` / `type` so a component may read
  /// them if it wants, and so round-tripping a config never loses fields this
  /// struct does not know about.
  nlohmann::json raw = nlohmann::json::object();

  /**
   * @brief Parse one entry.
   *
   * @throws std::runtime_error if `id` or `type` is missing or empty. Both are
   *         required because a component with neither can be neither
   *         constructed nor referenced by a teleop pair, and defaulting either
   *         would silently attach config to the wrong device.
   */
  static ComponentConfig from_json(const nlohmann::json& j) {
    ComponentConfig c;
    if (!j.contains("id") || !j.at("id").is_string() ||
        j.at("id").get<std::string>().empty()) {
      throw std::runtime_error(
        "ComponentConfig: every hardware.components entry requires a non-empty "
        "string 'id'");
    }
    if (!j.contains("type") || !j.at("type").is_string() ||
        j.at("type").get<std::string>().empty()) {
      throw std::runtime_error(
        "ComponentConfig: component '" + j.at("id").get<std::string>() +
        "' requires a non-empty string 'type' naming a registered hardware type");
    }
    c.id   = j.at("id").get<std::string>();
    c.type = j.at("type").get<std::string>();
    c.raw  = j;
    return c;
  }

  /// @brief The entry as given. Round-trips whatever was parsed.
  nlohmann::json to_json() const { return raw; }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__COMPONENT_CONFIG_HPP_
