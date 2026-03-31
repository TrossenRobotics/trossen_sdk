/**
 * @file keyboard_input_utils.hpp
 * @brief Terminal keyboard input utilities for interactive session control
 */

#ifndef TROSSEN_SDK__UTILS__KEYBOARD_INPUT_UTILS_HPP
#define TROSSEN_SDK__UTILS__KEYBOARD_INPUT_UTILS_HPP

#include <memory>

namespace trossen::utils {

/// @brief Named key press actions
enum class KeyPress {
  kNone,
  kRightArrow,
  kLeftArrow,
  kUpArrow,
  kDownArrow,
  kSpace,
  kEnter,
  kQ,
};

/**
 * @brief RAII guard that sets terminal to raw mode
 *
 * Disables line buffering and echo so individual keypresses can be
 * detected without waiting for Enter. Restores original terminal
 * settings on destruction. No-op if stdin is not a terminal.
 */
class RawModeGuard {
public:
  RawModeGuard();
  ~RawModeGuard();

  RawModeGuard(const RawModeGuard&) = delete;
  RawModeGuard& operator=(const RawModeGuard&) = delete;
  RawModeGuard(RawModeGuard&&) = delete;
  RawModeGuard& operator=(RawModeGuard&&) = delete;

  /// @brief Whether raw mode was successfully activated
  [[nodiscard]] bool is_active() const { return active_; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool active_{false};
};

/**
 * @brief Non-blocking poll for a keypress
 *
 * Returns immediately with the key that was pressed, or kNone if no key
 * is available. Must be called while a RawModeGuard is active to get
 * single-keypress detection.
 *
 * Handles multi-byte escape sequences (arrow keys) transparently.
 *
 * @return The key pressed, or KeyPress::kNone if nothing available
 * @note Not thread-safe. Call from a single thread only.
 */
KeyPress poll_keypress();

}  // namespace trossen::utils

#endif  // TROSSEN_SDK__UTILS__KEYBOARD_INPUT_UTILS_HPP
