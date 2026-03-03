/**
 * @file backend_utils.hpp
 * @brief Utility functions for backends
 */

#ifndef INCLUDE_TROSSEN_SDK_IO_BACKEND_UTILS_HPP_
#define INCLUDE_TROSSEN_SDK_IO_BACKEND_UTILS_HPP_

#include <filesystem>

namespace trossen::io::backends {

/**
 * @brief Expand a leading ~ in a path to the user's home directory
 * @param path The path string that may start with ~
 * @return The expanded path with ~ replaced by $HOME, or the original path
 *
 * Analogous to Python's os.path.expanduser(). Only expands a leading "~"
 * or "~/"; other occurrences of ~ are left unchanged.
 */
inline std::filesystem::path expand_user(const std::string& path) {
  if (path.empty() || path[0] != '~') {
    return std::filesystem::path(path);
  }
  // Only expand bare "~" or "~/..." (not "~user/...")
  if (path.size() > 1 && path[1] != '/') {
    return std::filesystem::path(path);
  }
  const char* home = std::getenv("HOME");
  if (!home) {
    return std::filesystem::path(path);
  }
  // "~" → home, "~/foo" → home / "foo"
  return std::filesystem::path(home) / path.substr(path.size() > 1 ? 2 : 1);
}

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
 * @return Generated dataset ID string
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

// Common constants used across multiple backends
inline constexpr int DEFAULT_ENCODER_THREADS = 1;
inline constexpr int DEFAULT_MAX_IMAGE_QUEUE = 0;
inline constexpr int DEFAULT_PNG_COMPRESSION_LEVEL = 3;
inline constexpr char DEFAULT_ROBOT_NAME[] = "trossen_ai_stationary";

}  // namespace trossen::io::backends

#endif  // INCLUDE_TROSSEN_SDK_IO_BACKEND_UTILS_HPP_
