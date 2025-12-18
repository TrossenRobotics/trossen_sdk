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

struct McapBackendConfig : public BaseConfig {
  std::string root{trossen::io::backends::get_default_root_path().string()};
  std::string robot_name{"/robot/joint_states"};
  int chunk_size_bytes{4 * 1024 * 1024};
  std::string compression{""};
  std::string dataset_id;
  int episode_index{0};

  std::string type() const override { return "mcap_backend"; }

  static  McapBackendConfig from_json(const nlohmann::json& j) {
    McapBackendConfig c;
    c.root = j.value("root", "");
    c.robot_name = j.value("robot_name", "/robot/joint_states");
    c.chunk_size_bytes = j.value("chunk_size_bytes", 4 * 1024 * 1024);
    c.compression = j.value("compression", "");
    c.dataset_id = j.at("dataset_id").get<std::string>();
    c.episode_index = j.value("episode_index", 0);

    return c;
  }
};

REGISTER_CONFIG(McapBackendConfig, "mcap_backend");

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__MCAP_BACKEND_CONFIG_HPP_
