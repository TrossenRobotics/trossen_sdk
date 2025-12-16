/**
 * @file session_manager_config.hpp
 * @brief Configuration for Session Manager
 */
#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__RUNTIME__SESSION_MANAGER_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__RUNTIME__SESSION_MANAGER_CONFIG

#include <optional>
#include <chrono>
#include "../../base_config.hpp"
// #include "../../json.hpp"
#include "../../config_registry.hpp"

struct SessionManagerConfig : public BaseConfig {
    std::optional<std::chrono::duration<double>> max_duration{std::chrono::seconds(20)};
    std::optional<uint32_t> max_episodes{std::nullopt};
    std::string backend_type{"mcap"};

    std::string type() const override { return "session_manager"; }

    static SessionManagerConfig from_json(const nlohmann::json& j) {
        SessionManagerConfig c;
        c.max_duration = j.contains("max_duration")
            ? std::optional<std::chrono::duration<double>>{
              std::chrono::duration<double>{j.at("max_duration").get<double>()}}
            : std::nullopt;
        c.max_episodes = j.contains("max_episodes") ?
            std::optional<uint32_t>{j.at("max_episodes").get<uint32_t>()} :
            std::nullopt;
        c.backend_type = j.value("backend_type", "mcap");

        return c;
    }
};

REGISTER_CONFIG(SessionManagerConfig, "session_manager");
#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__RUNTIME__SESSION_MANAGER_CONFIG_HPP_
