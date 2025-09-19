#ifndef TROSSEN_SDK_UTILS_CONSTANTS_HPP
#define TROSSEN_SDK_UTILS_CONSTANTS_HPP
#include <filesystem>

namespace trossen_sdk {

/// @brief Default root path for dataset storage
const std::filesystem::path DEFAULT_ROOT_PATH =
    std::filesystem::path(std::getenv("HOME")) / ".cache" / "trossen_dataset_collection_sdk";

/// @brief Format string for leader robot configuration file path
const std::string LEADER_ROBOT_CONFIG_FORMAT = "../config/{}_leader.json";

/// @brief Format string for follower robot configuration file path
const std::string FOLLOWER_ROBOT_CONFIG_FORMAT = "../config/{}.json";

/// @brief Position data name
const std::string POSITION = "position";
/// @brief Velocity data name
const std::string VELOCITY = "velocity";
/// @brief External effort data name
const std::string EXTERNAL_EFFORT = "external_effort";

/// @brief Leader model identifier
const std::string LEADER_MODEL = "leader";

/// @brief Follower model identifier
const std::string FOLLOWER_MODEL = "follower";

// Metadata related files

const std::string JSON_INFO = "info.json";
const std::string JSONL_EPISODES = "episodes.jsonl";
const std::string JSONL_EPISODE_STATS = "episodes_stats.jsonl";
const std::string JSONL_TASKS = "tasks.jsonl";

const std::string METADATA_DIR = "meta";
const std::string IMAGES_DIR = "images";
const std::string VIDEO_DIR = "videos";
const std::string DATA_PATH_DIR = "data";

// Versioning

const std::string CODEBASE_VERSION = "v2.1";
const std::string TROSSEN_SUBVERSION = "v1.0";

// Dataset paths

const std::string DATA_PATH = "data/chunk-{:03d}/episode_{:06d}.parquet";
const std::string VIDEO_PATH = "videos/chunk-{:03d}/{}/episode_{:06d}.mp4";
const std::string IMAGE_PATH = "images/chunk-{:03d}/{}/episode_{:06d}/image_{:06d}.jpg";

// Metadata Path Formats (For Compatibility with LeRobot)
const std::string DATA_PATH_META =
    "data/chunk-{episode_chunk:03d}/episode_{episode_index:06d}.parquet";
const std::string VIDEO_PATH_META =
    "videos/chunk-{episode_chunk:03d}/{video_key}/episode_{episode_index:06d}.mp4";

}  // namespace trossen_sdk

#endif  // TROSSEN_SDK_UTILS_CONSTANTS_HPP