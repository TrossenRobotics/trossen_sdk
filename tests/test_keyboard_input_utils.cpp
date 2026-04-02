/**
 * @file test_keyboard_input_utils.cpp
 * @brief Unit tests for KeyPress enum, RawModeGuard, and poll_keypress
 *
 * Tests work both in CI (stdin is not a terminal) and interactively
 * (stdin is a TTY) by branching on isatty() where behavior differs.
 */

#include <unistd.h>

#include "gtest/gtest.h"

#include "trossen_sdk/utils/keyboard_input_utils.hpp"

using trossen::utils::KeyPress;
using trossen::utils::RawModeGuard;
using trossen::utils::poll_keypress;

// KI-01: KeyPress::kNone exists and is the default/zero value
TEST(KeyboardInputUtilsTest, KeyPress_kNone_IsDefaultValue) {
  KeyPress key{};
  EXPECT_EQ(key, KeyPress::kNone);
  EXPECT_EQ(static_cast<int>(KeyPress::kNone), 0);
}

// KI-02: All KeyPress enum values are distinct
TEST(KeyboardInputUtilsTest, KeyPress_AllValuesDistinct) {
  KeyPress values[] = {
    KeyPress::kNone,
    KeyPress::kRightArrow,
    KeyPress::kLeftArrow,
    KeyPress::kUpArrow,
    KeyPress::kDownArrow,
    KeyPress::kSpace,
    KeyPress::kEnter,
    KeyPress::kQ,
  };

  constexpr size_t count = sizeof(values) / sizeof(values[0]);
  for (size_t i = 0; i < count; ++i) {
    for (size_t j = i + 1; j < count; ++j) {
      EXPECT_NE(values[i], values[j])
        << "KeyPress values at index " << i << " and " << j << " are equal";
    }
  }
}

// KI-03: RawModeGuard construction and destruction doesn't crash
TEST(KeyboardInputUtilsTest, RawModeGuard_ConstructDestruct_NoCrash) {
  EXPECT_NO_THROW({
    RawModeGuard guard;
  });
}

// KI-04: RawModeGuard::is_active() matches whether stdin is a terminal
TEST(KeyboardInputUtilsTest, RawModeGuard_IsActive_MatchesIsatty) {
  RawModeGuard guard;
  if (isatty(STDIN_FILENO)) {
    EXPECT_TRUE(guard.is_active());
  } else {
    EXPECT_FALSE(guard.is_active());
  }
}

// KI-05: poll_keypress returns kNone when no input available
TEST(KeyboardInputUtilsTest, PollKeypress_ReturnsNone_WhenNoInput) {
  // Skip when running interactively — buffered terminal input could interfere
  if (isatty(STDIN_FILENO)) {
    GTEST_SKIP() << "Skipping: stdin is a terminal";
  }
  KeyPress key = poll_keypress();
  EXPECT_EQ(key, KeyPress::kNone);
}
