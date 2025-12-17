/**
 * @file null_backend_config.hpp
 * @brief Configuration for Null backend
 */
#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__NULL_BACKEND_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__NULL_BACKEND_CONFIG_HPP_
#include "../../base_config.hpp"
#include "../../config_registry.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/configuration/global_config.hpp"

namespace trossen::configuration {

struct NullBackendConfig : public BaseConfig {
    std::string type() const override { return "null_backend"; }

    static NullBackendConfig from_json(const nlohmann::json& j) {
        NullBackendConfig c;
        return c;
    }
};


REGISTER_CONFIG(NullBackendConfig, "null_backend");
}  // namespace trossen::configuration
#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__NULL_BACKEND_CONFIG_HPP_
