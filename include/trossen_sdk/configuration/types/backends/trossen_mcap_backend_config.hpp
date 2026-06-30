/**
 * @file trossen_mcap_backend_config.hpp
 * @brief Configuration for TrossenMCAP backend
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__TROSSEN_MCAP_BACKEND_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__TROSSEN_MCAP_BACKEND_CONFIG_HPP_

#include "trossen_sdk/configuration/base_config.hpp"
#include "trossen_sdk/configuration/config_registry.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/io/backend_utils.hpp"

namespace trossen::configuration {

// TrossenMCAP backend specific constants
inline constexpr int TROSSEN_MCAP_DEFAULT_CHUNK_SIZE_BYTES = 4 * 1024 * 1024;
inline constexpr char TROSSEN_MCAP_DEFAULT_COMPRESSION[] = "";

struct TrossenMCAPBackendConfig : public BaseConfig {
  std::string root{trossen::io::backends::get_default_root_path().string()};
  std::string robot_name{trossen::io::backends::DEFAULT_ROBOT_NAME};
  int chunk_size_bytes{TROSSEN_MCAP_DEFAULT_CHUNK_SIZE_BYTES};
  std::string compression{TROSSEN_MCAP_DEFAULT_COMPRESSION};
  std::string dataset_id{trossen::io::backends::auto_generate_dataset_id()};
  // TODO(shantanuparab-tr): Remove episode index if not being used
  int episode_index{0};

  /// Write a /robot_description message to each MCAP file at recording start.
  bool include_robot_description{false};
  /// Embed mesh files as base64 data URIs inside the URDF (requires include_robot_description).
  bool include_meshes{false};
  /// Git ref (branch or tag) on TrossenRobotics/trossen_arm_description to download from.
  std::string robot_description_ref{"main"};
  /// URDF path within the description repo (e.g. "urdf/generated/stationary_ai.urdf").
  /// Leave empty to auto-resolve from robot_name.
  std::string urdf_variant{""};

  std::string type() const override { return "trossen_mcap_backend"; }

  static TrossenMCAPBackendConfig from_json(const nlohmann::json& j) {
    TrossenMCAPBackendConfig c;

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
    if (j.contains("include_robot_description")) {
      j.at("include_robot_description").get_to(c.include_robot_description);
    }
    if (j.contains("include_meshes")) j.at("include_meshes").get_to(c.include_meshes);
    if (j.contains("robot_description_ref")) {
      j.at("robot_description_ref").get_to(c.robot_description_ref);
    }
    if (j.contains("urdf_variant")) j.at("urdf_variant").get_to(c.urdf_variant);

    return c;
  }
};

REGISTER_CONFIG(TrossenMCAPBackendConfig, "trossen_mcap_backend");

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__TROSSEN_MCAP_BACKEND_CONFIG_HPP_
