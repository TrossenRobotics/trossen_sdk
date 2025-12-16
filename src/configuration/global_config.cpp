/**
* @file global_config.cpp
* @brief Global configuration manager implementation
*/
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/config_registry.hpp"

GlobalConfig& GlobalConfig::instance() {
    static GlobalConfig g;
    return g;
}

std::shared_ptr<BaseConfig> GlobalConfig::get(const std::string& key) const {
    auto it = config_map_.find(key);
    if (it == config_map_.end()) return nullptr;
    return it->second;
}

void GlobalConfig::load_from_json(const nlohmann::json& j) {
    for (auto& [key, value] : j.items()) {
        // Handle nested namespaces like cameras.wrist
        if (value.is_object() && !value.contains("type")) {
            for (auto& [subkey, subvalue] : value.items()) {
                auto cfg = ConfigRegistry::instance().create(subvalue);
                config_map_[key + "." + subkey] = cfg;
            }
        } else {
            auto cfg = ConfigRegistry::instance().create(value);
            config_map_[key] = cfg;
        }
    }
}
