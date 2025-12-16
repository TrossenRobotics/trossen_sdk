/**
 * @file config_registry.hpp
 * @brief Configuration registry for dynamic config type creation
 */
#ifndef TROSSEN_SDK__CONFIGURATION__CONFIG_REGISTRY_HPP_
#define TROSSEN_SDK__CONFIGURATION__CONFIG_REGISTRY_HPP_
#include <memory>
#include <unordered_map>
#include <functional>
#include <nlohmann/json.hpp>
#include "trossen_sdk/configuration/base_config.hpp"

class ConfigRegistry {
public:
    using BuilderFn = std::function<std::shared_ptr<BaseConfig>(const nlohmann::json&)>;

    static ConfigRegistry& instance() {
        static ConfigRegistry r;
        return r;
    }

    void register_type(const std::string& name, BuilderFn fn) {
        registry_[name] = fn;
    }

    std::shared_ptr<BaseConfig> create(const nlohmann::json& j) {
        std::string type = j.at("type");
        auto it = registry_.find(type);
        if (it == registry_.end()) {
            throw std::runtime_error("Unknown config type: " + type);
        }
        return it->second(j);
    }

private:
    std::unordered_map<std::string, BuilderFn> registry_;
};

#define REGISTER_CONFIG(Type, Name) \
    static bool reg_##Type = [](){ \
        ConfigRegistry::instance().register_type(Name, \
            [](const nlohmann::json& j){ return std::make_shared<Type>(Type::from_json(j)); }); \
        return true; \
    }();
#endif  // TROSSEN_SDK__CONFIGURATION__CONFIG_REGISTRY_HPP_
