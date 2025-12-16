#pragma once
#include "../../base_config.hpp"
// #include "../../json.hpp"
#include "../../config_registry.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/configuration/global_config.hpp"

struct NullBackendConfig : public BaseConfig {
    std::string type() const override { return "null_backend"; }

    static NullBackendConfig from_json(const nlohmann::json& j) {
        NullBackendConfig c;
        return c;
    }
};

REGISTER_CONFIG(NullBackendConfig, "null_backend");
