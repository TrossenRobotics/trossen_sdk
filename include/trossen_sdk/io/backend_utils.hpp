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

}  // namespace trossen::io::backends

#endif  // INCLUDE_TROSSEN_SDK_IO_BACKEND_UTILS_HPP_
