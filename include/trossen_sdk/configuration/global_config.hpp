#pragma once
#include <unordered_map>
#include <memory>
#include <string>
#include "i_config.hpp"
#include <nlohmann/json.hpp>

class GlobalConfig {
public:
    static GlobalConfig& instance();

    void load_from_json(const nlohmann::json& j);

    std::shared_ptr<IConfig> get(const std::string& key) const;

    template<typename T>
    std::shared_ptr<T> get_as(const std::string& key) const {
        return std::static_pointer_cast<T>(get(key));
    }

private:
    std::unordered_map<std::string, std::shared_ptr<IConfig>> config_map_;
};
