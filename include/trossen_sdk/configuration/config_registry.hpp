#pragma once
#include <memory>
#include <unordered_map>
#include <functional>
#include <nlohmann/json.hpp>
#include "i_config.hpp"

class ConfigRegistry {
public:
    using BuilderFn = std::function<std::shared_ptr<IConfig>(const nlohmann::json&)>;

    static ConfigRegistry& instance() {
        static ConfigRegistry r;
        return r;
    }

    void register_type(const std::string& name, BuilderFn fn) {
        registry_[name] = fn;
    }

    std::shared_ptr<IConfig> create(const nlohmann::json& j) {
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
            [](const nlohmann::json& j){ return std::make_shared<Type>(Type::from_json(j)); } \
        ); \
        return true; \
    }();
