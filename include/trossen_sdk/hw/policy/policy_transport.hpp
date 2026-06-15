/**
 * @file policy_transport.hpp
 * @brief Abstract transport interface for the PolicyClient.
 */

#ifndef TROSSEN_SDK__HW__POLICY__POLICY_TRANSPORT_HPP_
#define TROSSEN_SDK__HW__POLICY__POLICY_TRANSPORT_HPP_

#include <optional>

#include "nlohmann/json.hpp"

#include "trossen_sdk/hw/policy/action_chunk.hpp"
#include "trossen_sdk/hw/policy/observation.hpp"
#include "trossen_sdk/hw/policy/transport_status.hpp"

namespace trossen::hw::policy {

/**
 * @brief Async-first transport to a remote policy server.
 *
 * The client packs a neutral ``Observation``; the transport owns its wire
 * format end-to-end (encoding, protocol quirks, framing). Request/reply
 * servers (openpi) implement the pair with an internal worker thread and a
 * one-deep reply buffer; streaming servers (LeRobot async_inference) map it
 * onto their native send/receive paths.
 *
 * Hot-path contract — the inference loop drives a robot, so:
 * - ``push_observation`` and ``try_poll_chunk`` NEVER block and NEVER throw.
 *   A transport's internal failures are reported via ``status()``, which the
 *   client polls once per cycle (logging state transitions only).
 * - ``push_observation`` is latest-wins: if a previous observation is still
 *   in flight, the transport may drop the older one. A newer robot state
 *   always supersedes a stale one; queuing depth would feed the server
 *   history instead of reality.
 * - While unhealthy (``status() != kConnected``): pushes drop, polls return
 *   ``std::nullopt``.
 *
 * Threading: the client guarantees a single caller (its inference thread) for
 * the hot-path trio (``push_observation`` / ``try_poll_chunk`` / ``status``).
 * The lifecycle methods (``connect`` / ``close`` / ``server_metadata``) are
 * owner-serialized: the owner calls them only from its own setup/teardown,
 * never concurrently with each other or with the hot path. Implementations may
 * therefore assume ``connect`` and ``close`` do not race, and need not make
 * those two reentrant. Implementations may run internal threads, but this
 * interface is the only cross-thread boundary the client assumes.
 */
class PolicyTransport {
public:
  virtual ~PolicyTransport() = default;

  PolicyTransport(const PolicyTransport &) = delete;
  PolicyTransport & operator=(const PolicyTransport &) = delete;

  /**
   * @brief Open the underlying connection and complete the server handshake.
   *
   * Blocking is fine here (startup path, not the hot loop).
   * @throws std::runtime_error on any connection or handshake failure.
   */
  virtual void connect() = 0;

  /// Close the underlying connection. Idempotent; never throws.
  virtual void close() noexcept = 0;

  /// Server handshake metadata, populated by ``connect()``.
  [[nodiscard]] virtual const nlohmann::json & server_metadata() const = 0;

  /**
   * @brief Hand one observation to the transport for (eventual) delivery.
   *
   * Returns immediately. Latest-wins on backpressure. Dropped silently while
   * unhealthy (the drop is visible via ``status().failure_count``).
   */
  virtual void push_observation(const Observation & obs) noexcept = 0;

  /**
   * @brief Take the next decoded action chunk, if one has arrived.
   *
   * The returned chunk is fully stamped by the transport (``chunk_seq``
   * monotonic per connection, ``received_at`` at frame receipt).
   * ``std::nullopt`` means "nothing this poll" — normal, not an error.
   */
  [[nodiscard]] virtual std::optional<ActionChunk> try_poll_chunk() noexcept = 0;

  /// Current health summary; see TransportStatus for field contracts.
  [[nodiscard]] virtual TransportStatus status() const noexcept = 0;

protected:
  PolicyTransport() = default;
};

}  // namespace trossen::hw::policy

#endif  // TROSSEN_SDK__HW__POLICY__POLICY_TRANSPORT_HPP_
