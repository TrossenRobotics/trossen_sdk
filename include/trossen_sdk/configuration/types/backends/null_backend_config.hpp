#pragma once
#include "../../i_config.hpp"
// #include "../../json.hpp"
#include "../../config_registry.hpp"
#include "trossen_sdk/io/backend_utils.hpp"

struct NullBackendConfig : public IConfig {
    std::string uri;

    std::string type() const override { return "null_backend"; }

    static  NullBackendConfig from_json(const nlohmann::json& j) {
        NullBackendConfig c;
        c.uri = j.at("uri").get<std::string>();
        return c;
    }
};

REGISTER_CONFIG(NullBackendConfig, "null_backend");
