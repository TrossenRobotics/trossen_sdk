/**
 * @file lerobot_grpc_transport.hpp
 * @brief PolicyTransport speaking LeRobot async_inference over gRPC.
 *
 * PRIVATE header: the class is reachable only through the TransportRegistry
 * name ``lerobot_grpc``; nothing outside the SDK names this type. (That is
 * also why it may include generated protobuf headers — they never leak into
 * the public API surface.)
 *
 * Flow:
 *  - connect(): opens the channel to ``host:port``, runs the ``Ready`` liveness
 *    call, then ``SendPolicyInstructions`` with the pickled RemotePolicyConfig
 *    from policy_setup_bytes_().
 *  - push_observation(): pickles the observation and streams it on
 *    ``SendObservations`` (latest-wins on backpressure).
 *  - try_poll_chunk(): an internal receive loop polls ``GetActions`` and decodes
 *    each reply through the LerobotCodec into an ActionChunk.
 */

#ifndef TROSSEN_SDK__HW__POLICY__LEROBOT_GRPC_TRANSPORT_HPP_
#define TROSSEN_SDK__HW__POLICY__LEROBOT_GRPC_TRANSPORT_HPP_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "nlohmann/json.hpp"

#include "lerobot_transport_services.grpc.pb.h"

#include "trossen_sdk/hw/policy/lerobot_codec.hpp"
#include "trossen_sdk/hw/policy/policy_transport.hpp"

namespace trossen::hw::policy {

class LerobotGrpcTransport : public PolicyTransport {
public:
  /**
   * @param id              Owning PolicyClient's logical id (log prefixes /
   *                        errors).
   * @param target          gRPC target, ``host:port`` (validated by the
   *                        factory).
   * @param policy_config   Resolved RemotePolicyConfig declared to the server
   *                        at handshake (built + validated by the factory from
   *                        transport_config).
   * @param connect_timeout Per-step handshake budget (channel-ready wait and
   *                        each RPC). Generous by default — a real server may
   *                        JIT/load weights before answering Ready.
   * @param rpc_timeout     Deadline on each hot-path RPC (SendObservations,
   *                        GetActions). Short by default: GetActions is a
   *                        unary short-poll that returns immediately and
   *                        SendObservations streams one already-built payload,
   *                        so a few-second deadline never truncates a legit
   *                        call — it only bounds a wedged server so a worker
   *                        cannot block forever on the robot path.
   */
  LerobotGrpcTransport(std::string id, std::string target,
                       LerobotPolicyConfig policy_config,
                       std::chrono::milliseconds connect_timeout =
                         std::chrono::seconds(10),
                       std::chrono::milliseconds rpc_timeout =
                         std::chrono::seconds(5));
  ~LerobotGrpcTransport() override;

  /**
   * @brief Open the channel and run the async_inference handshake.
   *
   * Sequence (matching LeRobot's robot_client): wait for the channel to be
   * ready, call ``Ready(Empty)``, then ``SendPolicyInstructions(PolicySetup)``
   * with policy_setup_bytes_(). Each step has a deadline; any failure throws
   * with the gRPC status detail.
   */
  void connect() override;

  /// Shut the channel down. Idempotent; never throws.
  void close() noexcept override;

  /// LeRobot's handshake returns no metadata; always an empty object.
  [[nodiscard]] const nlohmann::json & server_metadata() const override;

  /// Pickle @p obs and hand it to the send loop (latest-wins on backpressure).
  /// Dropped silently while unhealthy; never blocks, never throws.
  void push_observation(const Observation & obs) noexcept override;

  /// Take the next decoded action chunk if the receive loop has one ready;
  /// ``std::nullopt`` otherwise. Never blocks, never throws.
  [[nodiscard]] std::optional<ActionChunk> try_poll_chunk() noexcept override;

  [[nodiscard]] TransportStatus status() const noexcept override;

  /// Test-only seam: inject a stub (e.g. a gmock MockAsyncInferenceStub) before
  /// connect(). When a stub is present connect() skips opening a real gRPC
  /// channel and runs the handshake against it, so the transport logic can be
  /// exercised with no channel/server (and no gRPC teardown race). Not part of
  /// the public API — this header is reachable only via the TransportRegistry.
  void set_stub_for_test(
    std::unique_ptr<transport::AsyncInference::StubInterface> stub) noexcept {
    stub_ = std::move(stub);
  }

private:
  /// Bytes for ``PolicySetup.data``: the pickled RemotePolicyConfig declaring
  /// this client's policy (type, features, rename map) to the server, emitted
  /// by the LerobotCodec from policy_config_.
  [[nodiscard]] std::vector<uint8_t> policy_setup_bytes_() const;

  /// Map a neutral Observation onto the LeRobot wire shape (codec POD):
  ///  - state flattened to per-joint ``"<motor>.pos"`` entries in layout order;
  ///    motor name from ``joint_names`` (positional ``"<group>_<i>"`` fallback);
  ///  - camera keys prefixed ``"observation.images."``; images HWC uint8 in
  ///    TRUE RGB (no BGR quirk — that is openpi-only);
  ///  - ``timestamp`` is ``captured_at`` as fractional seconds (monotonic; the
  ///    server uses it only relatively).
  /// The handshake ``rename_map`` is applied server-side, so keys here are raw.
  [[nodiscard]] static LerobotObservation map_observation_(const Observation & obs);

  /// Sender thread body: drain pending_obs_ (latest-wins), encode, stream.
  void send_loop_();

  /// Stream one pickled observation over a fresh ``SendObservations`` call,
  /// split into ``TransferState`` chunks (2 MB) exactly as LeRobot's
  /// send_bytes_in_chunks. Records its own failures; returns false on RPC error
  /// or a stream broken mid-send.
  bool send_observation_bytes_(const std::vector<uint8_t> & bytes);

  /// Receiver thread body: loop ``GetActions``, decode, publish ready_chunk_.
  void receive_loop_();

  void record_failure_(const std::string & why) noexcept;

  std::string id_;
  std::string target_;
  LerobotPolicyConfig policy_config_;
  std::chrono::milliseconds connect_timeout_;
  std::chrono::milliseconds rpc_timeout_;  ///< deadline for hot-path RPCs

  std::shared_ptr<grpc::Channel> channel_;
  // StubInterface (not the concrete Stub) so tests can inject a mock stub via
  // set_stub_for_test(); the production path assigns AsyncInference::NewStub().
  std::unique_ptr<transport::AsyncInference::StubInterface> stub_;

  nlohmann::json server_metadata_ = nlohmann::json::object();
  std::atomic<bool> connected_{false};

  // Send path (inference thread -> sender thread). pending_obs_ is one-deep,
  // latest-wins; the sender does all the slow streaming work off the hot path.
  std::thread sender_;
  std::mutex send_mu_;
  std::condition_variable send_cv_;
  bool sender_running_{false};               ///< guarded by send_mu_
  std::optional<Observation> pending_obs_;   ///< in-slot, latest-wins; send_mu_
  /// ClientContext of the in-flight SendObservations, for close() to TryCancel
  /// a blocked stream so the join cannot hang. nullptr when none is active.
  std::atomic<grpc::ClientContext *> active_send_ctx_{nullptr};

  // Receive path (receiver thread -> inference thread). The receiver actively
  // polls GetActions, so it uses an atomic flag rather than a condition var.
  std::thread receiver_;
  std::atomic<bool> receiver_running_{false};
  uint64_t chunk_seq_{0};                    ///< receiver thread is sole writer
  /// ClientContext of the in-flight GetActions, for close() to TryCancel.
  std::atomic<grpc::ClientContext *> active_get_ctx_{nullptr};

  mutable std::mutex mu_;
  uint64_t failure_count_{0};            ///< guarded by mu_; lifetime-monotonic
  std::string last_error_;               ///< guarded by mu_
  std::optional<ActionChunk> ready_chunk_;  ///< out-slot, newest wins; mu_
};

}  // namespace trossen::hw::policy

#endif  // TROSSEN_SDK__HW__POLICY__LEROBOT_GRPC_TRANSPORT_HPP_
