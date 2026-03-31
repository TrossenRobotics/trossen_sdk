/**
 * @file keyboard_input_utils.cpp
 * @brief Implementation of terminal keyboard input utilities
 */

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <mutex>

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include "trossen_sdk/utils/keyboard_input_utils.hpp"

namespace trossen::utils {

/// ASCII escape byte (start of terminal escape sequences)
static constexpr int kEscapeByte = 0x1B;

/// Maximum time (ms) to wait for continuation bytes of an escape sequence.
/// Terminal emulators typically send all bytes within 1ms, but we allow 50ms
/// to handle slow SSH connections.
static constexpr int kEscapeSequenceTimeoutMs = 50;

/// Short drain timeout (ms) for consuming leftover bytes from unrecognized
/// escape sequences (F-keys, Home, Delete, mouse, etc.)
static constexpr int kDrainTimeoutMs = 10;

// ─── RawModeGuard ────────────────────────────────────────────

/// Track active instances to prevent nested guards from corrupting terminal state.
static std::atomic<int> g_raw_mode_refcount{0};

/// Saved terminal settings for atexit restoration.
/// Best-effort: runs on normal exit (exit()/return from main), not on
/// fatal signals (SIGKILL, SIGABRT) that skip atexit handlers.
static termios g_saved_termios{};
static std::atomic<bool> g_termios_saved{false};

/// atexit handler: restores terminal if process exits while raw mode is active
static void restore_terminal_atexit() {
  if (g_termios_saved.load(std::memory_order_acquire)) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
  }
}

/// Register the atexit handler exactly once (thread-safe)
static void ensure_atexit_registered() {
  static std::once_flag flag;
  std::call_once(flag, []() { std::atexit(restore_terminal_atexit); });
}

struct RawModeGuard::Impl {
  termios original{};
};

RawModeGuard::RawModeGuard() {
  if (!isatty(STDIN_FILENO)) return;

  // Only the first guard saves and modifies terminal settings
  if (g_raw_mode_refcount.fetch_add(1) > 0) {
    active_ = true;  // piggyback on existing raw mode
    return;
  }

  impl_ = std::make_unique<Impl>();
  if (tcgetattr(STDIN_FILENO, &impl_->original) != 0) {
    g_raw_mode_refcount.fetch_sub(1);
    impl_.reset();
    return;
  }

  // Save for atexit safety net (restores terminal if process exits abnormally)
  g_saved_termios = impl_->original;
  g_termios_saved.store(true, std::memory_order_release);
  ensure_atexit_registered();

  // Non-blocking character-at-a-time input without echo
  termios raw = impl_->original;
  raw.c_lflag &= ~(ICANON | ECHO);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
    g_termios_saved.store(false, std::memory_order_release);
    g_raw_mode_refcount.fetch_sub(1);
    impl_.reset();
    return;
  }
  active_ = true;
}

RawModeGuard::~RawModeGuard() {
  if (!active_) return;

  // Only the last guard restores terminal settings, using the shared snapshot
  if (g_raw_mode_refcount.fetch_sub(1) == 1) {
    if (g_termios_saved.load(std::memory_order_acquire)) {
      tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
      g_termios_saved.store(false, std::memory_order_release);
    }
  }
}

// ─── poll_keypress ───────────────────────────────────────────

/// Check if a byte is available on stdin, with optional timeout in milliseconds.
/// Handles EINTR from signal delivery by retrying.
static bool has_input(int timeout_ms = 0) {
  pollfd pfd{STDIN_FILENO, POLLIN, 0};
  int ret;
  do {
    ret = poll(&pfd, 1, timeout_ms);
  } while (ret == -1 && errno == EINTR);
  if (ret <= 0) return false;
  return (pfd.revents & POLLIN) != 0;
}

/// Read a single byte from stdin, or -1 if unavailable/EOF.
static int read_byte() {
  char c;
  ssize_t n;
  do {
    n = read(STDIN_FILENO, &c, 1);
  } while (n == -1 && errno == EINTR);
  if (n == 1) return static_cast<unsigned char>(c);
  return -1;
}

/// Drain any remaining bytes from an unrecognized escape sequence.
static void drain_escape_sequence() {
  while (has_input(kDrainTimeoutMs)) {
    read_byte();
  }
}

KeyPress poll_keypress() {
  if (!has_input()) return KeyPress::kNone;

  int c = read_byte();
  if (c < 0) {
    // EOF or error after poll said readable — brief backoff to avoid hot spin
    usleep(1000);
    return KeyPress::kNone;
  }

  // Escape sequence (arrow keys, etc.) — wait briefly for continuation bytes
  if (c == kEscapeByte) {
    if (!has_input(kEscapeSequenceTimeoutMs)) return KeyPress::kNone;
    int c2 = read_byte();
    if (c2 != '[') {
      drain_escape_sequence();
      return KeyPress::kNone;
    }

    if (!has_input(kEscapeSequenceTimeoutMs)) return KeyPress::kNone;
    int c3 = read_byte();
    switch (c3) {
      case 'A': return KeyPress::kUpArrow;
      case 'B': return KeyPress::kDownArrow;
      case 'C': return KeyPress::kRightArrow;
      case 'D': return KeyPress::kLeftArrow;
      default:
        drain_escape_sequence();
        return KeyPress::kNone;
    }
  }

  // Single-byte keys
  switch (c) {
    case ' ':  return KeyPress::kSpace;
    case '\n': return KeyPress::kEnter;
    case '\r': return KeyPress::kEnter;
    case 'q':  return KeyPress::kQ;
    case 'Q':  return KeyPress::kQ;
    default:   return KeyPress::kNone;
  }
}

}  // namespace trossen::utils
