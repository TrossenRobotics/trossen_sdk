#pragma once
#include "../../i_config.hpp"
// #include "../../json.hpp"
#include "../../config_registry.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/configuration/global_config.hpp"

struct TrossenBackendConfig : public BaseConfig {
    std::string root{"/data/trossen"};
    int encoder_threads{1};
    int max_image_queue{0};
    int png_compression_level{3};
    std::string drop_policy{"DropNewest"};

    std::string type() const override { return "trossen_backend"; }

    static TrossenBackendConfig from_json(const nlohmann::json& j) {
        TrossenBackendConfig c;
        c.root = j.value("root", "");
        if (c.root.empty()) {
            c.root = trossen::io::backends::get_default_root_path().string();
        }
        c.encoder_threads = j.value("encoder_threads", 1);
        c.max_image_queue = j.value("max_image_queue", 0);
        c.png_compression_level = j.value("png_compression_level", 3);
        c.drop_policy = j.value("drop_policy", "DropNewest");

        return c;
    }
};

REGISTER_CONFIG(TrossenBackendConfig, "trossen_backend");
