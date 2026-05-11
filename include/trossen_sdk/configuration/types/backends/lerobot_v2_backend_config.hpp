/**
* @file lerobot_v2_backend_config.hpp
* @brief Configuration for LeRobotV2 backend
*/

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__LEROBOT_V2_BACKEND_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__LEROBOT_V2_BACKEND_CONFIG_HPP_

#include "trossen_sdk/configuration/base_config.hpp"
#include "trossen_sdk/configuration/config_registry.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/io/backend_utils.hpp"

namespace trossen::configuration {

// LeRobotV2 backend specific constants
inline constexpr bool LEROBOT_V2_DEFAULT_OVERWRITE_EXISTING = false;
inline constexpr bool LEROBOT_V2_DEFAULT_ENCODE_VIDEOS = false;
inline constexpr char LEROBOT_V2_DEFAULT_TASK_NAME[] = "perform a generic task";
inline constexpr char LEROBOT_V2_DEFAULT_REPOSITORY_ID[] = "TrossenRoboticsCommunity";
inline constexpr float LEROBOT_V2_DEFAULT_FPS = 30.0f;
inline constexpr int LEROBOT_V2_DEFAULT_EPISODE_INDEX = 0;
// Number of episodes stored per chunk folder (chunk-000, chunk-001, ...)
inline constexpr int LEROBOT_V2_DEFAULT_CHUNK_SIZE = 1000;
inline constexpr char LEROBOT_V2_DEFAULT_LICENSE[] = "apache-2.0";
// 0 = auto (70% of hardware_concurrency); positive = exact episode thread count
inline constexpr int LEROBOT_V2_DEFAULT_THREADS = 0;
// "auto" = GPU if available then CPU; "gpu" = force GPU; "cpu" = force CPU
inline constexpr char LEROBOT_V2_DEFAULT_ENCODER[] = "auto";

struct LeRobotV2BackendConfig : public BaseConfig {
  int encoder_threads{trossen::io::backends::DEFAULT_ENCODER_THREADS};
  int max_image_queue{trossen::io::backends::DEFAULT_MAX_IMAGE_QUEUE};
  int png_compression_level{trossen::io::backends::DEFAULT_PNG_COMPRESSION_LEVEL};
  bool overwrite_existing{LEROBOT_V2_DEFAULT_OVERWRITE_EXISTING};
  bool encode_videos{LEROBOT_V2_DEFAULT_ENCODE_VIDEOS};
  std::string task_name{LEROBOT_V2_DEFAULT_TASK_NAME};
  std::string repository_id{LEROBOT_V2_DEFAULT_REPOSITORY_ID};
  std::string dataset_id{trossen::io::backends::auto_generate_dataset_id()};
  std::string root{trossen::io::backends::get_default_root_path().string()};
  // TODO(shantanuparab-tr): DRemove episode index if not being used
  int episode_index{LEROBOT_V2_DEFAULT_EPISODE_INDEX};
  int chunk_size{LEROBOT_V2_DEFAULT_CHUNK_SIZE};
  std::string robot_name{trossen::io::backends::DEFAULT_ROBOT_NAME};
  float fps{LEROBOT_V2_DEFAULT_FPS};
  std::string license{LEROBOT_V2_DEFAULT_LICENSE};
  int threads{LEROBOT_V2_DEFAULT_THREADS};
  std::string encoder{LEROBOT_V2_DEFAULT_ENCODER};

  std::string type() const override { return "lerobot_v2_backend"; }

  static LeRobotV2BackendConfig from_json(const nlohmann::json& j) {
    LeRobotV2BackendConfig c;  // All defaults already set by member initializers

    // Only override if present in JSON
    if (j.contains("root")) {
      std::string raw_root;
      j.at("root").get_to(raw_root);
      c.root = trossen::io::backends::expand_user(raw_root).string();
    }
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
    if (j.contains("chunk_size")) j.at("chunk_size").get_to(c.chunk_size);
    if (j.contains("robot_name")) j.at("robot_name").get_to(c.robot_name);
    if (j.contains("fps")) j.at("fps").get_to(c.fps);
    if (j.contains("license")) j.at("license").get_to(c.license);
    if (j.contains("threads")) j.at("threads").get_to(c.threads);
    if (j.contains("encoder")) j.at("encoder").get_to(c.encoder);

    return c;
  }
};

REGISTER_CONFIG(LeRobotV2BackendConfig, "lerobot_v2_backend");

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__LEROBOT_V2_BACKEND_CONFIG_HPP_
