/**
 * @file global_config.hpp
 * @brief Global configuration manager
 */
#ifndef TROSSEN_SDK__CONFIGURATION__GLOBAL_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__GLOBAL_CONFIG_HPP_
#include <unordered_map>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include "trossen_sdk/configuration/base_config.hpp"

class GlobalConfig {
public:
    static GlobalConfig& instance();

    void load_from_json(const nlohmann::json& j);

    std::shared_ptr<BaseConfig> get(const std::string& key) const;

    template<typename T>
    std::shared_ptr<T> get_as(const std::string& key) const {
        auto base = get(key);
        if (!base) {
            throw std::runtime_error("Config key not found: " + key);
        }

        auto casted = std::dynamic_pointer_cast<T>(base);
        if (!casted) {
            throw std::runtime_error(
            "Config key '" + key + "' has wrong type");
        }

        return casted;
    }

private:
    std::unordered_map<std::string, std::shared_ptr<BaseConfig>> config_map_;
};
#endif  // TROSSEN_SDK__CONFIGURATION__GLOBAL_CONFIG_HPP_
