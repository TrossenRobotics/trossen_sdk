/**
* @file lerobot_backend_config.hpp
* @brief Configuration for LeRobot backend
*/

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__LEROBOT_BACKEND_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__LEROBOT_BACKEND_CONFIG_HPP_

#include "trossen_sdk/configuration/base_config.hpp"
#include "trossen_sdk/configuration/config_registry.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/io/backend_utils.hpp"

namespace trossen::configuration {

struct LeRobotBackendConfig : public BaseConfig {
  int encoder_threads{trossen::io::backends::DEFAULT_ENCODER_THREADS};
  int max_image_queue{trossen::io::backends::DEFAULT_MAX_IMAGE_QUEUE};
  int png_compression_level{trossen::io::backends::DEFAULT_PNG_COMPRESSION_LEVEL};
  bool overwrite_existing{trossen::io::backends::DEFAULT_OVERWRITE_EXISTING};
  bool encode_videos{trossen::io::backends::DEFAULT_ENCODE_VIDEOS};
  std::string task_name{trossen::io::backends::DEFAULT_TASK_NAME};
  std::string repository_id{trossen::io::backends::DEFAULT_REPOSITORY_ID};
  std::string dataset_id{trossen::io::backends::auto_generate_dataset_id()};
  std::string root{trossen::io::backends::get_default_root_path().string()};
  // TODO(shantanuparab-tr): DRemove episode index if not being used
  int episode_index{trossen::io::backends::DEFAULT_EPISODE_INDEX};
  std::string robot_name{trossen::io::backends::DEFAULT_ROBOT_NAME};
  float fps{trossen::io::backends::DEFAULT_FPS};

  std::string type() const override { return "lerobot_backend"; }

  static LeRobotBackendConfig from_json(const nlohmann::json& j) {
    LeRobotBackendConfig c;  // All defaults already set by member initializers

    // Only override if present in JSON
    if (j.contains("root")) j.at("root").get_to(c.root);
    if (j.contains("encoder_threads")) j.at("encoder_threads").get_to(c.encoder_threads);
    if (j.contains("max_image_queue")) j.at("max_image_queue").get_to(c.max_image_queue);
    if (j.contains("png_compression_level"))
      j.at("png_compression_level").get_to(c.png_compression_level);
    if (j.contains("overwrite_existing")) j.at("overwrite_existing").get_to(c.overwrite_existing);
    if (j.contains("encode_videos")) j.at("encode_videos").get_to(c.encode_videos);
    if (j.contains("task_name")) j.at("task_name").get_to(c.task_name);
    if (j.contains("repository_id")) j.at("repository_id").get_to(c.repository_id);
    if (j.contains("dataset_id")) j.at("dataset_id").get_to(c.dataset_id);
    if (j.contains("episode_index")) j.at("episode_index").get_to(c.episode_index);
    if (j.contains("robot_name")) j.at("robot_name").get_to(c.robot_name);
    if (j.contains("fps")) j.at("fps").get_to(c.fps);

    return c;
  }
};

REGISTER_CONFIG(LeRobotBackendConfig, "lerobot_backend");

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__LEROBOT_BACKEND_CONFIG_HPP_
