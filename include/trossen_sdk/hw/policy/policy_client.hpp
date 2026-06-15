/**
 * @file policy_client.hpp
 * @brief PolicyClient hardware/observer bridge and its per-arm Face adapters.
 */

#ifndef TROSSEN_SDK__HW__POLICY__POLICY_CLIENT_HPP_
#define TROSSEN_SDK__HW__POLICY__POLICY_CLIENT_HPP_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/types/hardware/policy_client_config.hpp"
#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/policy/action_chunk.hpp"
#include "trossen_sdk/hw/policy/policy_transport.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"
#include "trossen_sdk/observer/observer_base.hpp"

namespace trossen::hw::policy {

/**
 * @brief Per-cycle diagnostics emitted into the JSONL log alongside the
 *        observation payload.
 *
 * Carried separately from the on-wire observation so the openpi-format
 * payload (state / images / prompt) stays minimal. Populated in
 * ``pack_observation_`` and ``wait_for_fresh_observations_``; consumed by
 * ``log_request_``. Sized to fit on one cache line for cheap pass-by-value.
 */
struct ObservationDiagnostics {
  /// Per-record-id age, computed as ``cycle_start - rec.ts.monotonic`` at
  /// packing time. Same map key set as the subscriptions vector. Missing
  /// records (no entry in the cache) get a NaN entry so the consumer can
  /// distinguish "missing" from "fresh".
  std::unordered_map<std::string, double> ages_ms;
  /// ``max(ages_ms) - min(ages_ms)``; the cross-source temporal skew that
  /// the freshness barrier exists to bound. Zero when fewer than two
  /// subscriptions have a record.
  double skew_ms{0.0};
  /// Wall time the freshness barrier blocked before packing began.
  double freshness_wait_ms{0.0};
  /// Interval to the previous successful packing in ms; zero on the first
  /// cycle of an active window.
  double cycle_interval_ms{0.0};
};

/**
 * @brief Bridge between a remote policy server and the SDK teleop machinery.
 *
 * Multi-inherits ``HardwareComponent`` (for registry placement) and ``ObserverBase``
 * (for record-stream subscriptions). Owns one ``PolicyTransport``, one inference
 * thread, and N ``Face`` adapters that present per-arm slices of the latest action
 * chunk to existing ``TeleopController`` instances.
 *
 * Threading: ``ObserverBase`` spawns its subscription-dispatch worker via
 * ``start()``; ``on_start()`` additionally spawns the inference thread. Subscription
 * handlers and the inference loop share the per-record latest-record cache through
 * ``cache_mu_``; handlers never block on the network.
 *
 * Failure mode: server stalls or exceptions keep the inference thread alive and
 * leave the previous chunk in place; ``Face::read()`` returns the last commanded
 * row indefinitely (see ADR-004 hold-last-action).
 */
class PolicyClient
  : public hw::HardwareComponent,
    public observer::ObserverBase {
public:
  class Face;

  /**
   * @brief Construct with a logical id only; configuration is deferred to
   *        ``configure()``.
   *
   * Used by the hardware registry factory path: the registry constructs by id,
   * the data-collection layer then calls ``configure(json)`` which parses the
   * config, builds the transport, registers Faces, and adds subscriptions.
   *
   * @param id Logical hardware id (registry key and observer name).
   */
  explicit PolicyClient(std::string id);

  /**
   * @brief Construct with pre-built config + transport, registering Faces and
   *        subscriptions immediately.
   *
   * Intended for tests and direct programmatic construction (e.g. ``FakeTransport``).
   *
   * @param cfg       Pre-validated configuration (see PR 01).
   * @param transport Owned transport instance, not yet connected.
   *
   * @throws std::invalid_argument if @p transport is null.
   * @throws std::runtime_error if a ``leader_id`` is already registered or if
   *         the configuration violates the openpi observation contract.
   */
  PolicyClient(configuration::PolicyClientConfig cfg,
               std::unique_ptr<PolicyTransport> transport);

  ~PolicyClient() override;

  PolicyClient(const PolicyClient&) = delete;
  PolicyClient& operator=(const PolicyClient&) = delete;
  PolicyClient(PolicyClient&&) = delete;
  PolicyClient& operator=(PolicyClient&&) = delete;

  /**
   * @brief Parse @p cfg, build the transport, register Faces, and add
   *        subscriptions. Must be called at most once.
   *
   * No-op when the two-arg constructor was used (configuration already applied).
   *
   * @throws std::runtime_error if configuration parsing fails, if a ``leader_id``
   *         is already registered, or if the observation contract is violated.
   */
  void configure(const nlohmann::json& cfg) override;

  std::string get_type() const override { return "policy_client"; }

  /// Sum of ``joint_count`` across the configured ``joint_layout``.
  [[nodiscard]] int total_joint_count() const noexcept { return total_n_; }

  /// Row currently being commanded to all Faces; length is ``total_joint_count()``.
  [[nodiscard]] std::vector<float> current_command() const;

  /// Row-selection rate Faces and ``current_command()`` use when indexing the chunk.
  void set_control_rate_hz(double hz) noexcept;
  [[nodiscard]] double control_rate_hz() const noexcept {
    return control_rate_hz_.load(std::memory_order_acquire);
  }

  /// Faces registered by this client, in ``joint_layout`` order.
  [[nodiscard]] const std::vector<std::shared_ptr<Face>>& faces() const noexcept {
    return faces_;
  }

  /// Monotonic count of successful chunk publishes (test/diagnostic accessor).
  [[nodiscard]] uint64_t chunks_published() const noexcept {
    return chunks_published_.load(std::memory_order_acquire);
  }

  /// Snapshot of the currently-published action chunk; null if none has been
  /// received yet. Used by ``PolicyClientProducer`` to derive a chunk-aligned
  /// timestamp when ``use_device_time`` is enabled.
  [[nodiscard]] std::shared_ptr<const ActionChunk> latest_chunk() const noexcept {
    return chunk_slot_.peek();
  }

  /// Transport health snapshot. ``failure_count`` is the transport-lifetime
  /// failure counter (replaces the old ``round_trip_failures()``); episode
  /// abort-on-failure policy is the application's call, made through this
  /// accessor — the SDK itself only reports and holds last action.
  /// Default-constructed (kDisconnected) when no transport is configured.
  [[nodiscard]] TransportStatus transport_status() const noexcept {
    return transport_ ? transport_->status() : TransportStatus{};
  }

  /// Pause or resume the inference loop without tearing down the transport.
  /// When @p active is false the loop blocks (efficiently, on the condition
  /// variable) and skips all round-trips; the active chunk is also cleared so
  /// a stale action does not drive the mirror after resume. When transitioning
  /// from false to true the loop wakes and waits for the observation cache to
  /// be re-primed before sending the first request, matching the cold-start
  /// behavior. Default state is active (true) to preserve existing test
  /// behavior; lifecycle-aware examples should flip this around
  /// on_episode_started / on_episode_ended.
  void set_inference_active(bool active) noexcept;
  [[nodiscard]] bool inference_active() const noexcept {
    return inference_active_.load(std::memory_order_acquire);
  }

protected:
  /// Open the transport and spawn the inference thread.
  bool on_start() override;

  /// Signal, join the inference thread, then close the transport.
  void on_stop() override;

private:
  /// Shared wiring used by both ctors and the deferred ``configure()`` path.
  void init_(configuration::PolicyClientConfig cfg,
             std::unique_ptr<PolicyTransport> transport);

  void inference_loop_();

  /// Block until every subscribed @c record_id has delivered a record whose
  /// per-subscription delivery counter has advanced past the baseline captured
  /// at entry, or until ``cfg_.freshness_timeout_ms`` elapses. Guarantees that
  /// every record in the next observation snapshot was produced after this
  /// call began — and therefore within one producer period of every other
  /// record in the snapshot. Subsumes the legacy first-arrival prime: at
  /// startup/resume the baseline counters are zero, so the wait reduces to
  /// "every subscription has delivered at least once". On timeout, logs the
  /// stale record_ids (one-shot) and returns; the caller proceeds with the
  /// stalest available records.
  ///
  /// @return Wall time the call blocked, in milliseconds. Used by the
  ///         inference loop to populate ``ObservationDiagnostics``.
  double wait_for_fresh_observations_();

  /// Block until the firing instant of the currently-playing chunk
  /// (@c next_chunk_fire_target_): the drain-threshold θ point. At θ=0 this is
  /// the chunk's exhaustion instant, so the next observation captures the arm
  /// at its end-of-chunk pose (openpi's synchronous cadence); at θ>0 it fires
  /// earlier, overlapping inference with playback. No-op when no chunk is
  /// playing (first cycle, after pause/clear, or when the previous round-trip
  /// produced no chunk). Interruptible by pause or shutdown via @c inference_cv_.
  void wait_for_fire_point_();

  /// The Timestep Clock at instant @p now: the count of control-rate ticks
  /// since @c inference_epoch_. Zero before the epoch is set or when the
  /// control rate is unknown. Inference-thread only.
  [[nodiscard]] int64_t current_timestep_(
    std::chrono::steady_clock::time_point now) const noexcept;

  /// Pack the neutral Observation and populate per-record diagnostics
  /// (ages, skew) into @p diag. The skew/age math uses the steady-clock
  /// timestamps producers attach to every record. Wire shaping (flattening,
  /// channel quirks, transposes) is the transport's job, not done here.
  Observation pack_observation_(ObservationDiagnostics& diag);

  /// Extract one camera frame as a neutral Image: resized per @p sub, HWC,
  /// TRUE RGB (bgr8 records are converted; rgb8 pass through). Returns
  /// nullopt to skip the camera (bad record with no configured resize).
  std::optional<Observation::Image> pack_image_(
    const std::shared_ptr<data::RecordBase>& rec,
    const configuration::PolicyClientSubscriptionConfig& sub,
    const std::string& camera_key);

  /// All-black neutral Image placeholder for a missing camera frame.
  Observation::Image zero_image_(
    const std::string& camera_key, int width, int height) const;
  /// Gate, log, and publish one transport-decoded chunk into the slot.
  /// Consumes @p chunk (already validated and stamped by the transport);
  /// the only client-side check is chunk width vs the configured layout.
  void apply_chunk_(ActionChunk chunk, uint64_t request_seq, double rt_ms);

  void warn_once_(const std::string& key, const std::string& message);

  /// Open the JSONL log file declared in @p log_path. No-op when empty.
  /// Performs tilde expansion and creates parent directories. Logs but does
  /// not throw on failure (logging is best-effort).
  void open_log_file_(const std::string& log_path);

  /// Append one request/response JSONL record. Both helpers are no-ops when
  /// the log file is not open.
  void log_request_(uint64_t seq,
                    std::chrono::steady_clock::time_point t_send,
                    const Observation& obs,
                    const ObservationDiagnostics& diag);
  void log_response_(uint64_t seq,
                     std::chrono::steady_clock::time_point t_recv,
                     double rt_ms,
                     const std::shared_ptr<const ActionChunk>& chunk);

  /// Per-tick log emitted from @c Face::read on every control-loop sample.
  /// Carries the action row slice the follower actually executed, plus the
  /// chunk-playback metadata (chunk_seq, t_idx, saturated, blend_active)
  /// required to correlate the action stream with chunk boundaries and
  /// wait windows. Thread-safe (multiple faces poll concurrently).
  void log_tick_(const std::string& face_id,
                 std::chrono::steady_clock::time_point t,
                 const std::vector<float>& action,
                 const ChunkSlot::SampleInfo* info_ptr);

  bool configured_{false};
  configuration::PolicyClientConfig cfg_;
  std::unique_ptr<PolicyTransport> transport_;

  /// Subscriptions whose ``obs_key`` is ``state.*``, paired by position to
  /// ``cfg_.joint_layout`` entries.
  std::vector<const configuration::PolicyClientSubscriptionConfig*> state_subs_;
  /// Subscriptions whose ``obs_key`` is ``images.<camera>``; the camera key is
  /// the suffix after ``images.``.
  std::vector<const configuration::PolicyClientSubscriptionConfig*> image_subs_;
  std::vector<std::string> image_camera_keys_;

  mutable std::mutex cache_mu_;
  std::unordered_map<std::string, std::shared_ptr<data::RecordBase>> latest_records_;
  /// Per-subscription monotonic delivery counter, incremented every time the
  /// subscription handler stores a record. Read under @c cache_mu_; signaled
  /// to @c cache_fresh_cv_ on increment. The freshness barrier uses these as
  /// generation numbers to detect new deliveries without inspecting record
  /// content (so tests that reuse one record object work unchanged).
  std::unordered_map<std::string, uint64_t> cache_gen_;
  /// Signaled by every subscription handler after it bumps @c cache_gen_;
  /// waited on by @c wait_for_fresh_observations_. Distinct from
  /// @c inference_cv_ so a freshness wait does not race with the pause CV.
  std::condition_variable cache_fresh_cv_;
  /// Latched true the first time @c wait_for_fresh_observations_ returns
  /// (successfully or via timeout). Suppresses the "waiting for first
  /// observation" banner on every subsequent cycle. Touched only by the
  /// inference thread, so no synchronization is required.
  bool primed_observation_cache_{false};

  /// Expected exhaust instant of the last applied chunk, tracked in
  /// PolicyClient (not in ChunkSlot) because the slot's @c latest_ is
  /// only updated on the next face sample after a chunk is applied —
  /// a stale read here was the cause of the "wait skipped after cycle ~6"
  /// regression where the loop fell back to wall-clock cadence and
  /// re-introduced the mid-chunk observation problem (4.3). Updated in
  /// @c apply_chunk_ on every successful publish; consumed by
  /// @c wait_for_fire_point_ on the following cycle. Cleared to the default
  /// (epoch zero) by the inference thread when it wakes from a pause, and on
  /// shutdown. Inference-thread only: every read and write is on that thread
  /// (the pause path defers the clear here rather than writing from the caller).
  std::chrono::steady_clock::time_point next_chunk_exhaust_target_{};

  /// Instant at which to fire the next observation: the drain-threshold θ point
  /// of the last applied chunk, ``exhaust - θ·duration`` (= exhaust when θ=0).
  /// For θ>0 this is earlier than @c next_chunk_exhaust_target_, overlapping
  /// the next inference with the current chunk's playback. Cleared with the
  /// exhaust target on resume/shutdown. Consumed by @c wait_for_fire_point_.
  /// Inference-thread only.
  std::chrono::steady_clock::time_point next_chunk_fire_target_{};

  /// Epoch of the current active window (the Timestep Clock origin). Set when
  /// inference becomes active and re-set on resume, so any observation packed
  /// before a pause is stale by construction. Epoch-zero means "no active
  /// window yet". Inference-thread only (set in inference_loop_).
  std::chrono::steady_clock::time_point inference_epoch_{};

  ChunkSlot chunk_slot_;
  std::vector<std::shared_ptr<Face>> faces_;
  int total_n_{0};

  std::thread inference_thread_;
  std::atomic<bool> inference_running_{false};
  std::atomic<bool> inference_active_{true};
  std::mutex inference_mu_;
  std::condition_variable inference_cv_;

  std::atomic<double> control_rate_hz_{30.0};
  // Chunk identity (chunk_seq) is stamped by the transport, per connection —
  // the client deliberately has no counter of its own, so JSONL logs and
  // Face tick logs can never disagree on a chunk's number.
  std::atomic<uint64_t> chunks_published_{0};

  std::mutex warn_mu_;
  std::unordered_map<std::string, bool> warned_;

  /// JSONL log shared by the inference thread (request/response events) and
  /// every Face::read tick (tick events). The mutex serializes line writes
  /// so multi-arm setups don't interleave bytes mid-line.
  std::ofstream log_file_;
  std::mutex log_mu_;
  std::chrono::steady_clock::time_point log_t0_;
};

/**
 * @brief Per-arm leader Face: presents a sliced view of the owner's action chunk.
 *
 * Owns no state of its own besides ``[offset, count)``; ``read()`` delegates to
 * the owner's ``ChunkSlot``. ``write()`` is a no-op because Faces are leader-only.
 * Lifetime is tied to the owning ``PolicyClient``; Faces hold a raw back-pointer.
 */
class PolicyClient::Face
  : public hw::HardwareComponent,
    public hw::teleop::JointSpaceTeleop {
public:
  Face(PolicyClient* owner, std::string id, int joint_offset, int joint_count);

  /// No-op: Face is configured at construction by its owning PolicyClient.
  void configure(const nlohmann::json& cfg) override { (void)cfg; }

  std::string get_type() const override { return "policy_client_face"; }

  /// Returns the per-arm slice of the latest commanded action row.
  /// Honors hold-last-action: under any failure path (including allocation
  /// failure) returns a ``joint_count``-sized zero vector and never throws.
  std::vector<float> read() noexcept override;

  /// No-op: Faces are leader-only; commands are produced by the policy server.
  void write(const std::vector<float>& cmd) noexcept override { (void)cmd; }

  [[nodiscard]] int joint_offset() const noexcept { return joint_offset_; }
  [[nodiscard]] int joint_count() const noexcept { return joint_count_; }

  /// Clear the EMA filter history so the next ``read()`` returns the raw
  /// chunk row (no carry-over from before a pause). Called by
  /// ``PolicyClient::set_inference_active(false)`` so the filter doesn't
  /// blend the new chunk against pre-pause output on resume.
  void reset_output_filter() noexcept { prev_out_.clear(); }

private:
  PolicyClient* owner_;
  int joint_offset_;
  int joint_count_;
  /// Output of the previous ``read()`` after EMA application, used as the
  /// "previous output" term in ``out_t = α·row + (1-α)·prev_out``. Empty
  /// when no prior tick has run since construction / last reset; the next
  /// read passes the chunk row through unchanged and seeds this vector.
  /// Touched only by the teleop loop polling this face (single thread per
  /// face), so no synchronization is required.
  std::vector<float> prev_out_;
};

}  // namespace trossen::hw::policy

#endif  // TROSSEN_SDK__HW__POLICY__POLICY_CLIENT_HPP_
