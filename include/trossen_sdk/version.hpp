/**
 * @file version.hpp
 * @brief Version constants for the SDK
 */

#ifndef TROSSEN_SDK__CORE__VERSION_HPP
#define TROSSEN_SDK__CORE__VERSION_HPP

#include <cstdint>
#include <string>

namespace trossen::core {
/// @brief Major version number
inline constexpr uint32_t VERSION_MAJOR = 0;

/// @brief Minor version number
inline constexpr uint32_t VERSION_MINOR = 1;

/// @brief Patch version number
inline constexpr uint32_t VERSION_PATCH = 0;

/**
 * @brief Get version string in "vMAJOR.MINOR.PATCH" format
 *
 * @return Version string
 */
inline ::std::string version() {
  return "v" +
         ::std::to_string(VERSION_MAJOR) + "." +
         ::std::to_string(VERSION_MINOR) + "." +
         ::std::to_string(VERSION_PATCH);
}
}  // namespace trossen::core

#endif  // TROSSEN_SDK__CORE__VERSION_HPP
