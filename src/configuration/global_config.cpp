/**
* @file global_config.cpp
* @brief Global configuration manager implementation
*/

#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/config_registry.hpp"

trossen::configuration::GlobalConfig& trossen::configuration::GlobalConfig::instance() {
    static trossen::configuration::GlobalConfig g;
    return g;
}

std::shared_ptr<trossen::configuration::BaseConfig>
trossen::configuration::GlobalConfig::get(const std::string& key) const {
    auto it = config_map_.find(key);
    if (it == config_map_.end()) return nullptr;
    return it->second;
}

void trossen::configuration::GlobalConfig::load_from_json(const nlohmann::json& j) {
    for (auto& [key, value] : j.items()) {
        // Handle nested namespaces like cameras.wrist
        if (value.is_object() && !value.contains("type")) {
            for (auto& [subkey, subvalue] : value.items()) {
                auto cfg = trossen::configuration::ConfigRegistry::instance().create(subvalue);
                config_map_[key + "." + subkey] = cfg;
            }
        } else {
            auto cfg = trossen::configuration::ConfigRegistry::instance().create(value);
            config_map_[key] = cfg;
        }
    }
}
