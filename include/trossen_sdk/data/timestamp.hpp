/**
 * @file timestamp.hpp
 * @brief Dual clock timestamp used for ordering & correlation.
 */

#ifndef TROSSEN_SDK__DATA__TIMESTAMP_HPP
#define TROSSEN_SDK__DATA__TIMESTAMP_HPP

#include <chrono>
#include <cstdint>

namespace trossen::data {

/**
 * @brief Get current monotonic timestamp in ns
 */
static uint64_t now_mono_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

/**
 * @brief Get current realtime timestamp in ns since epoch
 */
static uint64_t now_real_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

/**
 * @brief Dual clock timestamp (monotonic + realtime).
 */
struct Timestamp {
  // Monotonic clock in ns since some unspecified starting point
  uint64_t monotonic_ns{0};

  // Wall clock UTC in ns since epoch
  uint64_t realtime_ns{0};
};

/**
 * @brief Create a Timestamp with both clocks set to now.
 */
inline Timestamp make_timestamp_now() {
  Timestamp ts;
  ts.monotonic_ns = now_mono_ns();
  ts.realtime_ns = now_real_ns();
  return ts;
}

} // namespace trossen::data

#endif // TROSSEN_SDK__DATA__TIMESTAMP_HPP
