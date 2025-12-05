#pragma once
#include "../../i_config.hpp"
// #include "../../json.hpp"
#include "../../config_registry.hpp"
#include "trossen_sdk/io/backend_utils.hpp"

struct TrossenBackendConfig : public IConfig {
    std::string output_dir;
    int encoder_threads{1};
    size_t max_image_queue{0};
    std::string drop_policy{"DropNewest"};
    int png_compression_level{3};

    std::string type() const override { return "trossen_backend"; }

    static  TrossenBackendConfig from_json(const nlohmann::json& j) {
        TrossenBackendConfig c;
        c.output_dir = j.value("output_dir", trossen::io::backends::get_default_root_path().string());
        c.encoder_threads = j.value("encoder_threads", 1);
        c.max_image_queue = j.value("max_image_queue", 0);
        c.drop_policy = j.value("drop_policy", "DropNewest");
        c.png_compression_level = j.value("png_compression_level", 3);

        return c;
    }
};

REGISTER_CONFIG(TrossenBackendConfig, "trossen_backend");
