// Copyright 2025 Trossen Robotics
#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT__LEROBOT_CONSTANTS_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT__LEROBOT_CONSTANTS_HPP_

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

const char JSON_INFO[] = "info.json";
const char JSONL_EPISODES[] = "episodes.jsonl";
const char JSONL_EPISODE_STATS[] = "episodes_stats.jsonl";
const char JSONL_TASKS[] = "tasks.jsonl";

const char METADATA_DIR[] = "meta";
const char IMAGES_DIR[] = "images";
const char VIDEO_DIR[] = "videos";
const char DATA_PATH_DIR[] = "data";

// Versioning

const char CODEBASE_VERSION[] = "v2.1";
const char TROSSEN_SUBVERSION[] = "v1.0";

// Dataset paths

const char DATA_PATH[] = "data/chunk-{:03d}/episode_{:06d}.parquet";
const char VIDEO_PATH[] = "videos/chunk-{:03d}/{}/episode_{:06d}.mp4";
const char IMAGE_PATH[] = "images/chunk-{:03d}/{}/episode_{:06d}/image_{:06d}.jpg";

// Metadata Path Formats (For Compatibility with LeRobot)
const char DATA_PATH_META[] = "data/chunk-{episode_chunk:03d}/episode_{episode_index:06d}.parquet";
const char VIDEO_PATH_META[] =
  "videos/chunk-{episode_chunk:03d}/{video_key}/"
  "episode_{episode_index:06d}.mp4";

// Display Images Target Size
const int DISPLAY_IMAGE_WIDTH = 640;
const int DISPLAY_IMAGE_HEIGHT = 480;

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT__LEROBOT_CONSTANTS_HPP_
