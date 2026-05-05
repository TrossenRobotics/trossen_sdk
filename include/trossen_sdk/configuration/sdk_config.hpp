/**
 * @file sdk_config.hpp
 * @brief Top-level SDK configuration aggregator
 *
 * SdkConfig is the single entry point for all SDK configuration.
 * Load it from a unified JSON file, optionally apply CLI overrides, and
 * it will populate the GlobalConfig singleton so all SDK components
 * (SessionManager, backends) pick up the correct settings automatically.
 *
 * Typical usage:
 * @code
 *   auto j = trossen::configuration::JsonLoader::load(config_path);
 *   auto overrides = cli.get_set_overrides();
 *   if (!overrides.empty()) {
 *     j = trossen::configuration::merge_overrides(j, overrides);
 *   }
 *   auto cfg = trossen::configuration::SdkConfig::from_json(j);
 *   cfg.populate_global_config();
 *
 *   // Now use cfg.hardware, cfg.producers, cfg.teleop directly
 *   // SessionManager and backends read from GlobalConfig automatically
 * @endcode
 *
 * JSON structure:
 * @code
 * {
 *   "_notes": ["..."],
 *   "robot_name": "trossen_stationary_ai",
 *   "hardware": {
 *     "arms":    { "<id>": { "ip_address": "...", "model": "...", "end_effector": "..." }, ... },
 *     "cameras": [ { "id": "...", "type": "realsense_camera", "serial_number": "...", ... }, ... ],
 *     "mobile_base": { "reset_odometry": false, "enable_torque": false }
 *   },
 *   "producers": [
 *     { "type": "trossen_arm",      "hardware_id": "<id>", "stream_id": "<id>", "poll_rate_hz": 30.0, "use_device_time": false },
 *     { "type": "realsense_camera", "hardware_id": "<id>", "stream_id": "<id>", "poll_rate_hz": 30.0, "encoding": "bgr8", "use_device_time": true },
 *     { "type": "slate_base",       "hardware_id": "slate_base", "stream_id": "slate_base", "poll_rate_hz": 30.0, "use_device_time": false }
 *   ],
 *   "teleop": {
 *     "enabled": true,
 *     "rate_hz": 1000.0,
 *     "pairs": [ { "leader": "<id>", "follower": "<id>", "space": "joint" }, ... ]
 *   },
 *   "backend": {
 *     "root": "~/trossen_data",
 *     "dataset_id": "my_dataset",
 *     "compression": "",
 *     "chunk_size_bytes": 4194304
 *   },
 *   "session": {
 *     "max_duration": 20.0,
 *     "max_episodes": 5,
 *     "backend_type": "trossen_mcap"
 *   }
 * }
 * @endcode
 *
 * @note Recording is currently supported in TrossenMCAP format only.
 *       Use the trossen_mcap_to_lerobot_v2 tool to convert recorded data to LeRobotV2 format.
 */

#ifndef TROSSEN_SDK__CONFIGURATION__SDK_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__SDK_CONFIG_HPP_

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/types/hardware/arm_config.hpp"
#include "trossen_sdk/configuration/types/hardware/camera_config.hpp"
#include "trossen_sdk/configuration/types/hardware/mobile_base_config.hpp"
#include "trossen_sdk/configuration/types/producers/producer_config.hpp"
#include "trossen_sdk/configuration/types/teleop_config.hpp"
#include "trossen_sdk/configuration/types/backends/trossen_mcap_backend_config.hpp"
#include "trossen_sdk/configuration/types/runtime/session_manager_config.hpp"

namespace trossen::configuration {

/**
 * @brief Hardware sub-configuration: arms, cameras, and optional mobile base
 */
struct HardwareConfig {
  /// @brief Named arm configs, keyed by logical id (e.g. "leader_left", "follower_right")
  std::unordered_map<std::string, ArmConfig> arms;

  /// @brief Ordered list of camera configs
  std::vector<CameraConfig> cameras;

  /// @brief Mobile base config (present only for mobile robots)
  std::optional<MobileBaseConfig> mobile_base;

  static HardwareConfig from_json(const nlohmann::json& j);
};


/**
 * @brief Top-level SDK configuration
 *
 * Aggregates hardware, producer, teleop, session, and backend configurations
 * into a single object loaded from a unified JSON file.
 *
 */
struct SdkConfig {
  /// @brief Human-readable robot name (used in backend metadata)
  std::string robot_name{"trossen_robot"};

  /// @brief Hardware component configurations
  HardwareConfig hardware;

  /// @brief Per-producer configurations (ordered list)
  std::vector<ProducerConfig> producers;

  /// @brief Teleoperation setup
  TeleoperationConfig teleop;

  /// @brief TrossenMCAP backend configuration (the only supported recording format)
  TrossenMCAPBackendConfig mcap_backend;

  /// @brief Session manager settings (episode duration, max episodes, backend selection)
  SessionManagerConfig session;

  /**
   * @brief Parse a SdkConfig from a unified JSON object
   *
   * @param j Unified SDK configuration JSON
   * @return Populated SdkConfig with defaults for any omitted fields
   */
  static SdkConfig from_json(const nlohmann::json& j);

  /**
   * @brief Load session, backend, and teleop configs into the GlobalConfig singleton
   *
   * Must be called before constructing a SessionManager or using the teleop
   * factory.
   */
  void populate_global_config() const;
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__SDK_CONFIG_HPP_
