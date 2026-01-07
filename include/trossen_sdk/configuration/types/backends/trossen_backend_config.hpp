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
  std::string root{trossen::io::backends::get_default_root_path().string()};
  int encoder_threads{trossen::io::backends::DEFAULT_ENCODER_THREADS};
  int max_image_queue{trossen::io::backends::DEFAULT_MAX_IMAGE_QUEUE};
  int png_compression_level{trossen::io::backends::DEFAULT_PNG_COMPRESSION_LEVEL};
  std::string drop_policy{trossen::io::backends::DEFAULT_DROP_POLICY};

  std::string type() const override { return "trossen_backend"; }

  static TrossenBackendConfig from_json(const nlohmann::json& j) {
    TrossenBackendConfig c;
    if (j.contains("root")) j.at("root").get_to(c.root);
    if (j.contains("encoder_threads")) j.at("encoder_threads").get_to(c.encoder_threads);
    if (j.contains("max_image_queue")) j.at("max_image_queue").get_to(c.max_image_queue);
    if (j.contains("png_compression_level"))
      j.at("png_compression_level").get_to(c.png_compression_level);
    if (j.contains("drop_policy")) j.at("drop_policy").get_to(c.drop_policy);

    return c;
  }
};

REGISTER_CONFIG(TrossenBackendConfig, "trossen_backend");

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__TROSSEN_BACKEND_CONFIG_HPP_
