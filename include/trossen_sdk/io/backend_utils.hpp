/**
 * @file backend_utils.hpp
 * @brief Utility functions for backends
 */

#ifndef INCLUDE_TROSSEN_SDK_IO_BACKEND_UTILS_HPP_
#define INCLUDE_TROSSEN_SDK_IO_BACKEND_UTILS_HPP_

#include <filesystem>

namespace trossen::io::backends {


/**
 * @enum Image queue drop policy when full
 */
enum class DropPolicy {
  /// @brief Drop newest incoming image
  DropNewest,

  /// @brief Drop oldest image in queue to make room for new one
  DropOldest,

  /// @brief Block until space is available (not implemented)
  // Block
};

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
 * @brief Convert drop policy string to enum
 *
 * @param s Drop policy string
 * @return DropPolicy enum value
 */
inline DropPolicy drop_policy_from_string(const std::string& s) {
  if (s == "DropNewest") return DropPolicy::DropNewest;
  if (s == "DropOldest") return DropPolicy::DropOldest;
  // Default/fallback
  return DropPolicy::DropNewest;
}

}  // namespace trossen::io::backends

#endif  // INCLUDE_TROSSEN_SDK_IO_BACKEND_UTILS_HPP_
