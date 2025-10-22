// Copyright 2025 Trossen Robotics
#ifndef INCLUDE_TROSSEN_SDK_UTILS_CONSTANTS_HPP_
#define INCLUDE_TROSSEN_SDK_UTILS_CONSTANTS_HPP_
#include <filesystem>
#include <string>
#include <sstream>
#include <iomanip>

namespace trossen_sdk {




/// @brief Default root path for dataset storage
const std::filesystem::path DEFAULT_ROOT_PATH =
    std::filesystem::path(std::getenv("HOME")) / ".cache" /
    "trossen_dataset_collection_sdk";

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
const char IMAGE_PATH[] =
    "images/chunk-{:03d}/{}/episode_{:06d}/image_{:06d}.jpg";

// Metadata Path Formats (For Compatibility with LeRobot)
const char DATA_PATH_META[] =
    "data/chunk-{episode_chunk:03d}/episode_{episode_index:06d}.parquet";
const char VIDEO_PATH_META[] =
    "videos/chunk-{episode_chunk:03d}/{video_key}/"
    "episode_{episode_index:06d}.mp4";

// Display Images Target Size
const int DISPLAY_IMAGE_WIDTH = 640;
const int DISPLAY_IMAGE_HEIGHT = 480;



/// @brief Simple string replacement function for config file paths
/// @param format_str The format string with {} placeholder
/// @param arg The string to replace {} with
/// @return Formatted string
inline std::string format_string(const std::string& format_str, const std::string& arg) {
    std::string result = format_str;
    size_t pos = result.find("{}");
    if (pos != std::string::npos) {
        result.replace(pos, 2, arg);
    }
    return result;
}

/// @brief Format episode name with zero-padded index
/// @param prefix The prefix string (e.g., "episode_")
/// @param episode_idx The episode index to format
/// @return Formatted episode string
inline std::string format_episode(const std::string& prefix, int episode_idx) {
    std::ostringstream oss;
    oss << prefix << std::setfill('0') << std::setw(6) << episode_idx;
    return oss.str();
}

/// @brief Format data path for parquet files
/// @param chunk_index The chunk index
/// @param episode_index The episode index
/// @return Formatted data path string
inline std::string format_data_path(int chunk_index, int episode_index) {
    std::ostringstream oss;
    oss << "data/chunk-" << std::setfill('0') << std::setw(3) << chunk_index
        << "/episode_" << std::setfill('0') << std::setw(6) << episode_index << ".parquet";
    return oss.str();
}

/// @brief Format image path for camera images
/// @param chunk_index The chunk index
/// @param camera_name The camera name
/// @param episode_index The episode index
/// @param frame_index The frame index
/// @return Formatted image path string
inline std::string format_image_path(int chunk_index, const std::string& camera_name,
                                   int episode_index, int frame_index) {
    std::ostringstream oss;
    oss << "images/chunk-" << std::setfill('0') << std::setw(3) << chunk_index
        << "/" << camera_name << "/episode_" << std::setfill('0') << std::setw(6) << episode_index
        << "/image_" << std::setfill('0') << std::setw(6) << frame_index << ".jpg";
    return oss.str();
}


}  // namespace trossen_sdk

#endif  // INCLUDE_TROSSEN_SDK_UTILS_CONSTANTS_HPP_
