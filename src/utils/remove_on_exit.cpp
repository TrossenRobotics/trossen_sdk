/**
 * @file remove_on_exit.cpp
 * @brief Implementation of the RemoveOnExit file guard.
 */

#include "trossen_sdk/utils/remove_on_exit.hpp"

#include <filesystem>
#include <system_error>
#include <utility>

namespace trossen::utils {

RemoveOnExit::RemoveOnExit(std::filesystem::path path) : path_(std::move(path)) {}

RemoveOnExit::~RemoveOnExit() {
  if (path_.empty()) return;
  std::error_code ec;
  std::filesystem::remove(path_, ec);
}

void RemoveOnExit::release() {
  path_.clear();
}

const std::filesystem::path& RemoveOnExit::path() const {
  return path_;
}

}  // namespace trossen::utils
