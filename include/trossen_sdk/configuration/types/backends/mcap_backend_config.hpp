/**
 * @file mcap_backend_config.hpp
 * @brief Configuration for MCAP backend
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__MCAP_BACKEND_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__MCAP_BACKEND_CONFIG_HPP_

#include "trossen_sdk/configuration/base_config.hpp"
#include "trossen_sdk/configuration/config_registry.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/io/backend_utils.hpp"

namespace trossen::configuration {

// MCAP backend specific constants
inline constexpr int MCAP_DEFAULT_CHUNK_SIZE_BYTES = 4 * 1024 * 1024;
inline constexpr char MCAP_DEFAULT_COMPRESSION[] = "";

struct McapBackendConfig : public BaseConfig {
  std::string root{trossen::io::backends::get_default_root_path().string()};
  std::string robot_name{trossen::io::backends::DEFAULT_ROBOT_NAME};
  int chunk_size_bytes{MCAP_DEFAULT_CHUNK_SIZE_BYTES};
  std::string compression{MCAP_DEFAULT_COMPRESSION};
  std::string dataset_id{trossen::io::backends::auto_generate_dataset_id()};
  // TODO(shantanuparab-tr): Remove episode index if not being used
  int episode_index{0};

  std::string type() const override { return "mcap_backend"; }

  static  McapBackendConfig from_json(const nlohmann::json& j) {
    McapBackendConfig c;

    // Only override if present in JSON
    if (j.contains("root")) {
      std::string raw_root;
      j.at("root").get_to(raw_root);
      c.root = trossen::io::backends::expand_user(raw_root).string();
    }
    if (j.contains("robot_name")) j.at("robot_name").get_to(c.robot_name);
    if (j.contains("chunk_size_bytes")) j.at("chunk_size_bytes").get_to(c.chunk_size_bytes);
    if (j.contains("compression")) j.at("compression").get_to(c.compression);
    if (j.contains("dataset_id")) j.at("dataset_id").get_to(c.dataset_id);
    if (j.contains("episode_index")) j.at("episode_index").get_to(c.episode_index);

    return c;
  }
};

REGISTER_CONFIG(McapBackendConfig, "mcap_backend");

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__MCAP_BACKEND_CONFIG_HPP_
