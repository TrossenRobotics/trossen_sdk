/**
 * @file trossen_backend_config.hpp
 * @brief Configuration for Trossen backend
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__TROSSEN_BACKEND_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__TROSSEN_BACKEND_CONFIG_HPP_

#include "trossen_sdk/configuration/base_config.hpp"
#include "trossen_sdk/configuration/config_registry.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/io/backend_utils.hpp"

namespace trossen::configuration {

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

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__TROSSEN_BACKEND_CONFIG_HPP_
