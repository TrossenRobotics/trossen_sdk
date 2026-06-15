/**
 * @file transport_status.hpp
 * @brief Health summary a PolicyTransport exposes to the polling client.
 *
 * The transport hot path (push_observation / try_poll_chunk) is noexcept and
 * non-blocking, so failures cannot surface as exceptions there. Instead the
 * transport keeps this summary current and the client polls status() once per
 * inference cycle, logging state TRANSITIONS only (warn-once style).
 */

#ifndef TROSSEN_SDK__HW__POLICY__TRANSPORT_STATUS_HPP_
#define TROSSEN_SDK__HW__POLICY__TRANSPORT_STATUS_HPP_

#include <cstdint>
#include <string>

namespace trossen::hw::policy {

struct TransportStatus
{
  enum class State
  {
    kConnected,      ///< healthy; pushes/polls are being serviced
    kReconnecting,   ///< self-healing (e.g. gRPC channel backoff); pushes drop
    kDisconnected,   ///< gone for good until the owner reconnects/restarts
  };

  State state{State::kDisconnected};

  /// Monotonic over the transport's lifetime, NEVER reset: a polling client
  /// computes failures-in-window as (now - last_sampled), which a resettable
  /// counter would corrupt (lost updates between samples).
  uint64_t failure_count{0};

  /// Empty when healthy; otherwise the most recent failure, for log readers.
  std::string last_error;
};

}  // namespace trossen::hw::policy

#endif  // TROSSEN_SDK__HW__POLICY__TRANSPORT_STATUS_HPP_
