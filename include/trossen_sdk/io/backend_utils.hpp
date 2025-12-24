/**
 * @file backend_utils.hpp
 * @brief Utility functions for backends
 */

#ifndef INCLUDE_TROSSEN_SDK_IO_BACKEND_UTILS_HPP_
#define INCLUDE_TROSSEN_SDK_IO_BACKEND_UTILS_HPP_

#include <filesystem>

namespace trossen::io::backends {

/**
 * @brief Safely get the default root path for dataset storage
 */
inline std::filesystem::path get_default_root_path() {
  const char* home = std::getenv("HOME");
  if (home) {
    return std::filesystem::path(home) / ".cache" / "trossen_sdk";
  } else {
    // Fallback to current directory if HOME is not set
    return std::filesystem::path(".") / ".cache" / "trossen_sdk";
  }
}

/**
 * @brief Auto-generate a dataset ID based on the current timestamp
 */
inline std::string auto_generate_dataset_id() {
  // Generate a timestamp-based dataset ID
  auto now = std::chrono::system_clock::now();
  auto time_t_now = std::chrono::system_clock::to_time_t(now);
  std::ostringstream oss;
  // Format: dataset_YYYYMMDD_HHMMSS in local time
  oss << "dataset_" << std::put_time(std::localtime(&time_t_now), "%Y%m%d_%H%M%S");
  return oss.str();
}

// Constants for default variables
constexpr int DEFAULT_ENCODER_THREADS = 1;
constexpr int DEFAULT_MAX_IMAGE_QUEUE = 0;
constexpr int DEFAULT_PNG_COMPRESSION_LEVEL = 3;
constexpr bool DEFAULT_OVERWRITE_EXISTING = false;
constexpr bool DEFAULT_ENCODE_VIDEOS = false;
constexpr char DEFAULT_TASK_NAME[] = "perform a generic task";
constexpr char DEFAULT_REPOSITORY_ID[] = "TrossenRoboticsCommunity";
constexpr char DEFAULT_ROBOT_NAME[] = "trossen_ai_stationary";
constexpr float DEFAULT_FPS = 30.0f;
constexpr int DEFAULT_CHUNK_SIZE_BYTES = 4 * 1024 * 1024;
constexpr char DEFAULT_COMPRESSION[] = "";
constexpr char DEFAULT_DROP_POLICY[] = "DropNewest";
constexpr int DEFAULT_EPISODE_INDEX = 0;

}  // namespace trossen::io::backends

#endif  // INCLUDE_TROSSEN_SDK_IO_BACKEND_UTILS_HPP_
