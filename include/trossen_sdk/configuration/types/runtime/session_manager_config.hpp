/**
 * @file session_manager_config.hpp
 * @brief Configuration for Session Manager
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__RUNTIME__SESSION_MANAGER_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__RUNTIME__SESSION_MANAGER_CONFIG_HPP_

#include <chrono>
#include <optional>

#include "trossen_sdk/configuration/base_config.hpp"
#include "trossen_sdk/configuration/config_registry.hpp"

namespace trossen::configuration {

struct SessionManagerConfig : public BaseConfig {
  std::optional<std::chrono::duration<double>> max_duration{std::chrono::seconds(20)};
  std::optional<uint32_t> max_episodes{std::nullopt};
  std::string backend_type{"trossen_mcap"};
  /// Reset duration between episodes.
  /// Positive = countdown timer (seconds). Zero = no wait (skip reset).
  /// nullopt (field absent) = infinite wait for user input.
  std::optional<std::chrono::duration<double>> reset_duration{std::nullopt};

  std::string type() const override { return "session_manager"; }

  static SessionManagerConfig from_json(const nlohmann::json& j) {
    SessionManagerConfig c;

    if (j.contains("max_duration")) {
      c.max_duration = std::chrono::duration<double>{j.at("max_duration").get<double>()};
    }
    if (j.contains("max_episodes")) {
      c.max_episodes = j.at("max_episodes").get<uint32_t>();
    }
    if (j.contains("backend_type")) {
      j.at("backend_type").get_to(c.backend_type);
    }
    if (j.contains("reset_duration")) {
      double val = j.at("reset_duration").get<double>();
      if (val > 0.0) {
        c.reset_duration = std::chrono::duration<double>{val};
      } else {
        // Zero or negative = no wait (skip reset phase entirely).
        // To get infinite wait, omit the field or set to null.
        c.reset_duration = std::chrono::duration<double>{0.0};
      }
    }

    return c;
  }
};

REGISTER_CONFIG(SessionManagerConfig, "session_manager");

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__RUNTIME__SESSION_MANAGER_CONFIG_HPP_
