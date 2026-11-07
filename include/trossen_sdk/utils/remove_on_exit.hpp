/**
 * @file remove_on_exit.hpp
 * @brief Scope guard that deletes a file when it goes out of scope.
 */

#ifndef TROSSEN_SDK__UTILS__REMOVE_ON_EXIT_HPP_
#define TROSSEN_SDK__UTILS__REMOVE_ON_EXIT_HPP_

#include <filesystem>

namespace trossen::utils {

/**
 * @brief Deletes a file on destruction unless released.
 *
 * Holds a temporary or in-progress file, so every return path, thrown exceptions included,
 * removes it. A missing file is not an error.
 */
class RemoveOnExit {
 public:
  /**
   * @brief Guard a file.
   *
   * @param path File to remove when the guard is destroyed.
   */
  explicit RemoveOnExit(std::filesystem::path path);

  /// @brief Remove the file unless release() was called.
  ~RemoveOnExit();

  RemoveOnExit(const RemoveOnExit&) = delete;
  RemoveOnExit& operator=(const RemoveOnExit&) = delete;
  RemoveOnExit(RemoveOnExit&&) = delete;
  RemoveOnExit& operator=(RemoveOnExit&&) = delete;

  /// @brief Keep the file: the guard no longer removes it.
  void release();

  /// @return The guarded file, or an empty path once released.
  const std::filesystem::path& path() const;

 private:
  /// @brief File to remove; empty once released.
  std::filesystem::path path_;
};

}  // namespace trossen::utils

#endif  // TROSSEN_SDK__UTILS__REMOVE_ON_EXIT_HPP_
