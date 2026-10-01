/**
 * @file control_config.hpp
 * @brief Configuration for an operator control-surface component
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__CONTROL_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__CONTROL_CONFIG_HPP_

#include <stdexcept>
#include <string>

#include "nlohmann/json.hpp"

namespace trossen::configuration {

/**
 * @brief Configuration for one operator control
 *
 * A control binds a physical control surface to a software effect. Three exist:
 * one publishes a handle's joystick and buttons, one turns a stick into base
 * velocity, one turns buttons into session events. What they share is not that
 * they are inputs — the base mapper emits commands — but that each exists
 * because a human is holding something. None is recorded and none appears in
 * `producers`, which is why they sit apart from arms and cameras rather than
 * among them.
 *
 * The @p type field selects which component is created via HardwareRegistry,
 * exactly as CameraConfig::type does. Unlike a camera there is no majority type
 * to default to, so it is required.
 *
 * Every other key is passed through verbatim.
 *
 * JSON format, keyed by component id:
 * @code
 * "controls": {
 *   "glide_inputs": {
 *     "type": "glide_arm_input",
 *     "arms": ["glide_left", "glide_right"]
 *   }
 * }
 * @endcode
 */
struct ControlConfig {
  /// @brief Hardware registry key — e.g. "glide_arm_input". Required.
  std::string type{};

  /// @brief Every key other than `type`, forwarded to
  /// HardwareComponent::configure() untouched.
  ///
  /// The three types share no settings beyond the discriminator — one takes a
  /// list of arm ids, one a pair of nested axis blocks, one a poll rate and a
  /// button table — so modelling any of them here would mean a member per key of
  /// one type, unread by the other two.
  nlohmann::json extra{};

  static ControlConfig from_json(const nlohmann::json& j) {
    if (!j.is_object()) {
      throw std::runtime_error("ControlConfig: entry must be an object");
    }
    if (!j.contains("type") || !j.at("type").is_string() ||
        j.at("type").get<std::string>().empty()) {
      throw std::runtime_error(
        "ControlConfig: 'type' is required and must be a non-empty string "
        "naming a registered hardware type (e.g. \"glide_arm_input\")");
    }

    ControlConfig c;
    j.at("type").get_to(c.type);
    // Parse-and-erase rather than a known-keys list: with the discriminator as
    // the only modelled field, a new control type needs no edit here, and there
    // is no list to fall out of sync with.
    c.extra = j;
    c.extra.erase("type");
    return c;
  }

  /// @brief Produce JSON suitable for HardwareComponent::configure()
  ///
  /// `type` is omitted, matching CameraConfig: the registry takes it as a
  /// separate argument, so repeating it here would put two copies in play.
  nlohmann::json to_json() const {
    return extra.is_object() ? extra : nlohmann::json::object();
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__CONTROL_CONFIG_HPP_
