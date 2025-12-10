#pragma once
#include "../../i_config.hpp"
// #include "../../json.hpp"
#include "../../config_registry.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/configuration/global_config.hpp"

struct NullBackendConfig : public IConfig {
    std::string uri{"/data/trossen"};

    std::string type() const override { return "null_backend"; }

    static NullBackendConfig from_json(const nlohmann::json& j) {
        NullBackendConfig c;
        c.uri = j.value("uri", "");
        if (c.uri.empty()) {
            c.uri = trossen::io::backends::get_default_root_path().string();
        }
        return c;
    }
};

REGISTER_CONFIG(NullBackendConfig, "null_backend");
