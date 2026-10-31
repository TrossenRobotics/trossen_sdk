/**
 * @file lerobot_v2_constants.hpp
 * @brief Constants for the LeRobot v2.1 dataset layout
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2__LEROBOT_V2_CONSTANTS_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2__LEROBOT_V2_CONSTANTS_HPP_

namespace trossen::io::backends::lerobot_v2 {

// Metadata related files

/// @brief Info JSON filename
inline constexpr char JSON_INFO[] = "info.json";
/// @brief Episodes JSONL filename
inline constexpr char JSONL_EPISODES[] = "episodes.jsonl";
/// @brief Episode statistics JSONL filename
inline constexpr char JSONL_EPISODE_STATS[] = "episodes_stats.jsonl";
/// @brief Tasks JSONL filename
inline constexpr char JSONL_TASKS[] = "tasks.jsonl";

/// Directory names

/// @brief Metadata directory
inline constexpr char METADATA_DIR[] = "meta";

/// @brief Images directory
inline constexpr char IMAGES_DIR[] = "images";

/// @brief Videos directory
inline constexpr char VIDEO_DIR[] = "videos";

/// @brief Data directory
inline constexpr char DATA_PATH_DIR[] = "data";

// Versioning

/// @brief Codebase version
inline constexpr char CODEBASE_VERSION[] = "v2.1";

/// @brief Trossen SDK subversion
inline constexpr char TROSSEN_SUBVERSION[] = "v1.0";

// Metadata Path Formats (For Compatibility with LeRobotV2)

/// @brief Data path format for metadata
inline constexpr char DATA_PATH_META[] =
  "data/chunk-{episode_chunk:03d}/episode_{episode_index:06d}.parquet";

/// @brief Video path format for metadata
inline constexpr char VIDEO_PATH_META[] =
  "videos/chunk-{episode_chunk:03d}/{video_key}/"
  "episode_{episode_index:06d}.mp4";

}  // namespace trossen::io::backends::lerobot_v2

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2__LEROBOT_V2_CONSTANTS_HPP_
