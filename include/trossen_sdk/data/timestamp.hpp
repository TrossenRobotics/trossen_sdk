/**
 * @file timestamp.hpp
 * @brief Dual clock timestamp used for ordering & correlation.
 */

#ifndef TROSSEN_SDK__DATA__TIMESTAMP_HPP
#define TROSSEN_SDK__DATA__TIMESTAMP_HPP

#include <chrono>
#include <cstdint>

namespace trossen::data {

/// @brief Nanoseconds per second
const uint64_t S_TO_NS = 1'000'000'000ull;

/**
 * @brief Time specification with seconds and nanoseconds components
 */
struct Timespec {
  /// @brief Seconds
  int64_t sec{0};

  /// @brief Nanoseconds (0 <= nsec < 1e9)
  uint32_t nsec{0};

  /**
   * @brief Convert to total nanoseconds
   *
   * @return Total nanoseconds in this time spec
   */
  uint64_t to_ns() const {
    return static_cast<uint64_t>(sec) * S_TO_NS + nsec;
  }

  /**
   * @brief Create from total nanoseconds
   *
   * @param total_ns Total nanoseconds
   * @return Timespec instance
   */
  static Timespec from_ns(uint64_t total_ns) {
    Timespec ts;
    ts.sec = static_cast<int64_t>(total_ns / S_TO_NS);
    ts.nsec = static_cast<uint32_t>(total_ns % S_TO_NS);
    return ts;
  }
};

/**
 * @brief Dual clock timestamp (monotonic + realtime) with (sec, nsec) representation
 */
struct Timestamp {
  /// @brief Monotonic clock (steady_clock)
  Timespec monotonic{};
  /// @brief Wall clock UTC (system_clock)
  Timespec realtime{};
};

/**
 * @brief Get current monotonic time as Timespec
 *
 * @return Current monotonic time
 * @note This can be used in place of device time if not provided
 */
inline Timespec now_mono() {
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
  return Timespec::from_ns(static_cast<uint64_t>(ns));
}

/**
 * @brief Get current realtime as Timespec
 *
 * @return Current realtime
 */
inline Timespec now_real() {
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  return Timespec::from_ns(static_cast<uint64_t>(ns));
}

/**
 * @brief Create a Timestamp with both clocks set to now
 *
 * @return Current Timestamp
 */
inline Timestamp make_timestamp_now() {
  Timestamp ts;
  ts.monotonic = now_mono();
  ts.realtime = now_real();
  return ts;
}

}  // namespace trossen::data

#endif  // TROSSEN_SDK__DATA__TIMESTAMP_HPP
