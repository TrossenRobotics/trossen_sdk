/**
 * @file lerobot_v3_backend_config.hpp
 * @brief Configuration for the LeRobot v3.0 converter/writer.
 *
 * Mirrors LeRobotV2BackendConfig but adds the v3-specific aggregation knobs
 * (chunks_size, data/video file size thresholds). Registered under the type
 * string "lerobot_v3_backend" so the converter accepts
 * `--set lerobot_v3_backend.<key>=...` overrides.
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__LEROBOT_V3_BACKEND_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__LEROBOT_V3_BACKEND_CONFIG_HPP_

#include "trossen_sdk/configuration/base_config.hpp"
#include "trossen_sdk/configuration/config_registry.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_v3/lerobot_v3_constants.hpp"

namespace trossen::configuration {

// LeRobot v3 backend defaults
inline constexpr bool LEROBOT_V3_DEFAULT_OVERWRITE_EXISTING = false;
inline constexpr bool LEROBOT_V3_DEFAULT_ENCODE_VIDEOS = true;
inline constexpr char LEROBOT_V3_DEFAULT_TASK_NAME[] = "perform a generic task";
inline constexpr char LEROBOT_V3_DEFAULT_REPOSITORY_ID[] = "TrossenRoboticsCommunity";
inline constexpr float LEROBOT_V3_DEFAULT_FPS = 30.0f;
inline constexpr char LEROBOT_V3_DEFAULT_LICENSE[] = "apache-2.0";

struct LeRobotV3BackendConfig : public BaseConfig {
  int encoder_threads{trossen::io::backends::DEFAULT_ENCODER_THREADS};
  int max_image_queue{trossen::io::backends::DEFAULT_MAX_IMAGE_QUEUE};
  int png_compression_level{trossen::io::backends::DEFAULT_PNG_COMPRESSION_LEVEL};
  bool overwrite_existing{LEROBOT_V3_DEFAULT_OVERWRITE_EXISTING};
  bool encode_videos{LEROBOT_V3_DEFAULT_ENCODE_VIDEOS};
  std::string task_name{LEROBOT_V3_DEFAULT_TASK_NAME};
  std::string repository_id{LEROBOT_V3_DEFAULT_REPOSITORY_ID};
  std::string dataset_id{trossen::io::backends::auto_generate_dataset_id()};
  std::string root{trossen::io::backends::get_default_root_path().string()};
  std::string robot_name{trossen::io::backends::DEFAULT_ROBOT_NAME};
  float fps{LEROBOT_V3_DEFAULT_FPS};
  std::string license{LEROBOT_V3_DEFAULT_LICENSE};

  // v3 aggregation knobs
  int chunks_size{trossen::io::backends::lerobot_v3::DEFAULT_CHUNK_SIZE};
  int data_files_size_in_mb{trossen::io::backends::lerobot_v3::DEFAULT_DATA_FILE_SIZE_IN_MB};
  int video_files_size_in_mb{trossen::io::backends::lerobot_v3::DEFAULT_VIDEO_FILE_SIZE_IN_MB};

  // When true, emit the native lerobot_trossen bimanual WidowX AI schema:
  //   * joint feature names as `<side>_joint_<i>.pos` / `<side>_left_carriage_joint.pos`
  //   * camera keys `cam_*` (instead of `camera_*`)
  //   * depth cameras encoded as lerobot-0.6.0-native HEVC `gray12le` (12-bit log-quant)
  // Off by default so other robots/converters keep their positional naming + av1 output.
  bool native_widowxai_schema{false};

  std::string type() const override { return "lerobot_v3_backend"; }

  static LeRobotV3BackendConfig from_json(const nlohmann::json& j) {
    LeRobotV3BackendConfig c;  // member initializers provide defaults

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
    if (j.contains("robot_name")) j.at("robot_name").get_to(c.robot_name);
    if (j.contains("fps")) j.at("fps").get_to(c.fps);
    if (j.contains("license")) j.at("license").get_to(c.license);
    if (j.contains("chunks_size")) j.at("chunks_size").get_to(c.chunks_size);
    if (j.contains("data_files_size_in_mb"))
      j.at("data_files_size_in_mb").get_to(c.data_files_size_in_mb);
    if (j.contains("video_files_size_in_mb"))
      j.at("video_files_size_in_mb").get_to(c.video_files_size_in_mb);
    if (j.contains("native_widowxai_schema"))
      j.at("native_widowxai_schema").get_to(c.native_widowxai_schema);

    return c;
  }
};

REGISTER_CONFIG(LeRobotV3BackendConfig, "lerobot_v3_backend");

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__LEROBOT_V3_BACKEND_CONFIG_HPP_
