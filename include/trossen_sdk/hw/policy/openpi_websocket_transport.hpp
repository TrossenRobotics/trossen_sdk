/**
 * @file openpi_websocket_transport.hpp
 * @brief WebSocket transport speaking the openpi msgpack-numpy wire protocol.
 */

#ifndef TROSSEN_SDK__HW__POLICY__OPENPI_WEBSOCKET_TRANSPORT_HPP_
#define TROSSEN_SDK__HW__POLICY__OPENPI_WEBSOCKET_TRANSPORT_HPP_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

#include "trossen_sdk/hw/policy/policy_transport.hpp"

namespace ix {
class WebSocket;
struct WebSocketMessage;
}  // namespace ix

namespace trossen::hw::policy {

/**
 * @brief Concrete ``PolicyTransport`` driving openpi over a single WebSocket.
 *
 * openpi is request/reply, so the async push/poll contract is implemented with
 * one internal worker thread and two one-deep slots:
 *
 *   push_observation() ──> pending_obs_ (latest-wins overwrite)
 *                              │ worker: pack to openpi json, round_trip_(),
 *                              ▼         decode reply
 *                          ready_chunk_ (newest wins)
 *   try_poll_chunk()   <──────┘
 *
 * The worker owns all blocking and all throwing; the interface methods are
 * noexcept and return immediately. A round-trip failure increments
 * ``failure_count`` and (matching the previous die-on-disconnect behavior)
 * the transport reports kDisconnected until close()/connect() cycles it.
 *
 * Wire protocol (see ADR-004 §5.1):
 *  - On connect, the server immediately pushes one msgpack-encoded
 *    ``server_metadata`` dict that ``connect()`` recv's once and stores.
 *  - Each request sends ``msgpack(obs_json)`` as one binary frame and blocks
 *    until one frame arrives in reply. A binary reply is the action chunk; a
 *    text reply is a server-side error.
 *  - Training quirk: images are sent CHW uint8 with channels swapped to BGR
 *    (labeled RGB on the wire) — the packer owns this; see pack_observation_json_.
 */
class OpenpiWebsocketTransport : public PolicyTransport {
public:
  /**
   * @brief Construct the transport (does not open the socket).
   * @param url              Full WebSocket URL (e.g. ``ws://host:port``).
   * @param api_key          Optional API key sent as
   *                         ``Authorization: Api-Key <key>``.
   * @param request_timeout  Upper bound on how long a single observation
   *                         round-trip waits for the server's reply frame.
   *                         Generous vs real inference latency by default — it
   *                         exists only to break a permanent stall against a
   *                         wedged/half-open server, not to bound normal calls.
   */
  OpenpiWebsocketTransport(std::string url, std::optional<std::string> api_key,
                           std::chrono::milliseconds request_timeout =
                             std::chrono::seconds(30));

  ~OpenpiWebsocketTransport() override;

  void connect() override;
  void close() noexcept override;
  [[nodiscard]] const nlohmann::json & server_metadata() const override;

  void push_observation(const Observation & obs) noexcept override;
  [[nodiscard]] std::optional<ActionChunk> try_poll_chunk() noexcept override;
  [[nodiscard]] TransportStatus status() const noexcept override;

  /// True between a successful ``connect()`` and either ``close()`` or a
  /// server-initiated close/error frame.
  [[nodiscard]] bool connected() const noexcept {return connected_.load();}

private:
  // --- synchronous request/reply core (pre-refactor logic, now private) -----
  /// Send one packed observation and block until the server's reply frame.
  /// Called only from the worker thread. Throws on any failure.
  nlohmann::json round_trip_(const nlohmann::json & obs);

  void on_message_(const ix::WebSocketMessage & msg);

  /// Reset the per-round-trip reply slot under reply_mu_.
  void clear_reply_();

  // --- neutral <-> openpi wire translation (worker thread only) -------------
  /// Pack the neutral Observation into the openpi json payload: state groups
  /// concatenated to one f32 ndarray; images transposed HWC->CHW with the
  /// BGR-as-RGB training quirk applied; task mapped to "prompt".
  [[nodiscard]] nlohmann::json pack_observation_json_(const Observation & obs) const;

  /// Decode the server reply's "actions" ndarray ([T, N], f32/f64) into a
  /// stamped ActionChunk. Returns nullopt (and records a failure) on any
  /// shape/dtype mismatch.
  [[nodiscard]] std::optional<ActionChunk> decode_reply_(const nlohmann::json & reply);

  // --- worker thread ---------------------------------------------------------
  void worker_loop_();
  void record_failure_(const std::string & why) noexcept;

  std::string url_;
  std::optional<std::string> api_key_;
  std::chrono::milliseconds request_timeout_;  ///< reply-wait ceiling, round_trip_

  std::unique_ptr<ix::WebSocket> ws_;
  nlohmann::json server_metadata_;
  std::atomic<bool> connected_{false};

  // Socket-callback handoff (ws thread <-> worker thread).
  std::mutex reply_mu_;
  std::condition_variable reply_cv_;
  std::vector<uint8_t> reply_bin_;
  std::string reply_text_;
  bool reply_is_text_{false};
  bool reply_ready_{false};
  bool reply_closed_{false};
  std::string close_reason_;

  // Worker handoff (client thread <-> worker thread). Distinct from reply_mu_:
  // that one serializes the socket callback, this one the public interface.
  std::thread worker_;
  mutable std::mutex work_mu_;
  std::condition_variable work_cv_;
  bool worker_running_{false};            ///< guarded by work_mu_
  std::optional<Observation> pending_obs_;  ///< in-slot, latest-wins
  std::optional<ActionChunk> ready_chunk_;  ///< out-slot, newest wins
  uint64_t chunk_seq_{0};                 ///< worker thread only
  uint64_t failure_count_{0};             ///< guarded by work_mu_
  std::string last_error_;                ///< guarded by work_mu_
};

}  // namespace trossen::hw::policy

#endif  // TROSSEN_SDK__HW__POLICY__OPENPI_WEBSOCKET_TRANSPORT_HPP_
