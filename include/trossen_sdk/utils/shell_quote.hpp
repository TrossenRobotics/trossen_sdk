/**
 * @file shell_quote.hpp
 * @brief Quote a string as one POSIX shell word, for the ffmpeg and ffprobe commands.
 */

#ifndef TROSSEN_SDK__UTILS__SHELL_QUOTE_HPP_
#define TROSSEN_SDK__UTILS__SHELL_QUOTE_HPP_

#include <filesystem>
#include <string>

namespace trossen::utils {

/**
 * @brief Quote @p arg so a POSIX shell reads it as one literal word.
 *
 * The result is wrapped in single quotes, inside which the shell expands nothing, and
 * each single quote in @p arg is written as `'\''`. ffmpeg's concat list files use the
 * same quoting.
 *
 * @param arg Any string, e.g. a path containing spaces or `$`.
 * @return The quoted word.
 */
inline std::string shell_quote(const std::string& arg) {
  std::string out = "'";
  for (const char c : arg) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

/// @brief shell_quote() of a path's native string.
inline std::string shell_quote(const std::filesystem::path& path) {
  return shell_quote(path.string());
}

}  // namespace trossen::utils

#endif  // TROSSEN_SDK__UTILS__SHELL_QUOTE_HPP_
