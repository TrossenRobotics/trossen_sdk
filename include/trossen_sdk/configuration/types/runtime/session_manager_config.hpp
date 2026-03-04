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

    return c;
  }
};

REGISTER_CONFIG(SessionManagerConfig, "session_manager");

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__RUNTIME__SESSION_MANAGER_CONFIG_HPP_
