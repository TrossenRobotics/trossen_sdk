/**
 * @file timestamp.hpp
 * @brief Multi-clock timestamp used for ordering & correlation.
 */

#ifndef TROSSEN_SDK__DATA__TIMESTAMP_HPP
#define TROSSEN_SDK__DATA__TIMESTAMP_HPP

#include <chrono>
#include <cstdint>

namespace trossen::data {

/// @brief Nanoseconds per second
const uint64_t S_TO_NS = 1'000'000'000ull;

/// @brief Nanoseconds per microsecond
const uint64_t US_TO_NS = 1'000ull;

/// @brief Nanoseconds per millisecond
const uint64_t MS_TO_NS = 1'000'000ull;

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
 * @brief Clock a device timestamp is expressed in.
 *
 * A device timestamp is only comparable to another timestamp from the same clock.
 * Recording which clock produced it is what makes it usable downstream: an uptime
 * counter and a UTC epoch are both plain integers and cannot be told apart by value.
 */
enum class DeviceClock : uint8_t {
  /// @brief No device timestamp was available; the `device` field is meaningless.
  None = 0,
  /// @brief Nanoseconds since the device powered on or was configured. Needs an
  ///        offset against a host clock before it can be compared across devices, and
  ///        can restart (a reconfigure) or wrap (a narrow hardware counter). A backward
  ///        jump starts a new segment.
  Uptime = 1,
  /// @brief Nanoseconds since the Unix epoch, as reported by the device.
  Epoch = 2,
  /// @brief Device clock the vendor SDK has already mapped onto the host realtime
  ///        clock, in nanoseconds since the Unix epoch.
  HostMapped = 3,
};

/**
 * @brief Multi-clock timestamp (monotonic + realtime + device) with (sec, nsec) parts
 *
 * `monotonic` and `realtime` are read on the host at the moment the sample is
 * received, so they measure delivery. `device` is the instant the device itself
 * reports for the sample, which for a camera is the exposure and for an arm is the
 * controller's own sample time. The three are kept separate because a delivery time
 * cannot substitute for a capture time when the delivery path stalls.
 */
struct Timestamp {
  /// @brief Monotonic clock (steady_clock), read on the host at delivery
  Timespec monotonic{};
  /// @brief Wall clock UTC (system_clock), read on the host at delivery
  Timespec realtime{};
  /// @brief Device-reported time for this sample, valid only when `device_clock` is
  ///        not DeviceClock::None
  Timespec device{};
  /// @brief Which clock `device` is expressed in
  DeviceClock device_clock{DeviceClock::None};

  /// @brief Whether this record carries a usable device timestamp
  bool has_device() const { return device_clock != DeviceClock::None; }
};

/**
 * @brief Name of a device clock, for metadata and logging
 *
 * @param clock Clock to name
 * @return Lowercase identifier, stable across releases
 */
inline const char* to_string(DeviceClock clock) {
  switch (clock) {
    case DeviceClock::Uptime: return "uptime";
    case DeviceClock::Epoch: return "epoch";
    case DeviceClock::HostMapped: return "host_mapped";
    case DeviceClock::None:
    default: return "none";
  }
}

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
