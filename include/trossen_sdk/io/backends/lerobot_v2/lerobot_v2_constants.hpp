/**
 * @file lerobot_v2_constants.hpp
 * @brief Constants for LeRobotV2 backend
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2__LEROBOT_V2_CONSTANTS_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2__LEROBOT_V2_CONSTANTS_HPP_

namespace trossen::io::backends {

/// @brief Format string for leader robot configuration file path
const char LEADER_ROBOT_CONFIG_FORMAT[] = "config/{}_leader.json";

/// @brief Format string for follower robot configuration file path
const char FOLLOWER_ROBOT_CONFIG_FORMAT[] = "config/{}.json";

/// @brief Position data name
const char POSITION[] = "position";

/// @brief Velocity data name
const char VELOCITY[] = "velocity";

/// @brief External effort data name
const char EXTERNAL_EFFORT[] = "external_effort";

/// @brief Leader model identifier
const char LEADER_MODEL[] = "leader";

/// @brief Follower model identifier
const char FOLLOWER_MODEL[] = "follower";

// Metadata related files

/// @brief Info JSON filename
const char JSON_INFO[] = "info.json";
/// @brief Episodes JSONL filename
const char JSONL_EPISODES[] = "episodes.jsonl";
/// @brief Episode statistics JSONL filename
const char JSONL_EPISODE_STATS[] = "episodes_stats.jsonl";
/// @brief Tasks JSONL filename
const char JSONL_TASKS[] = "tasks.jsonl";

/// Directory names

/// @brief Metadata directory
const char METADATA_DIR[] = "meta";

/// @brief Images directory
const char IMAGES_DIR[] = "images";

/// @brief Videos directory
const char VIDEO_DIR[] = "videos";

/// @brief Data directory
const char DATA_PATH_DIR[] = "data";

// Versioning

/// @brief Codebase version
const char CODEBASE_VERSION[] = "v2.1";

/// @brief Trossen SDK subversion
const char TROSSEN_SUBVERSION[] = "v1.0";

// Dataset paths

/// @brief Data path format
const char DATA_PATH[] = "data/chunk-{:03d}/episode_{:06d}.parquet";

/// @brief Video path format
const char VIDEO_PATH[] = "videos/chunk-{:03d}/{}/episode_{:06d}.mp4";

/// @brief Image path format
const char IMAGE_PATH[] = "images/chunk-{:03d}/{}/episode_{:06d}/image_{:06d}.jpg";

// Metadata Path Formats (For Compatibility with LeRobotV2)

/// @brief Data path format for metadata
const char DATA_PATH_META[] = "data/chunk-{episode_chunk:03d}/episode_{episode_index:06d}.parquet";

/// @brief Video path format for metadata
const char VIDEO_PATH_META[] =
  "videos/chunk-{episode_chunk:03d}/{video_key}/"
  "episode_{episode_index:06d}.mp4";

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2__LEROBOT_V2_CONSTANTS_HPP_
