/**
 * @file policy_client.cpp
 * @brief Implementation of PolicyClient and PolicyClient::Face.
 */

#include "trossen_sdk/hw/policy/policy_client.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "opencv2/core.hpp"
#include "opencv2/imgproc.hpp"

#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/policy/transport_registry.hpp"

namespace trossen::hw::policy {

namespace {

constexpr const char* kObsPrefixState = "state.";
constexpr const char* kObsPrefixImages = "images.";

// EXPECTED_CAMERAS from openpi/src/openpi/policies/aloha_policy.py:40 — the
// server rejects any image key outside this set.
const std::vector<std::string>& expected_cameras() {
  static const std::vector<std::string> kCams = {
    "cam_high", "cam_low", "cam_left_wrist", "cam_right_wrist"};
  return kCams;
}

bool is_expected_camera(const std::string& key) {
  for (const auto& c : expected_cameras()) {
    if (c == key) {
      return true;
    }
  }
  return false;
}

}  // namespace

// State shared (by shared_ptr) between a PolicyClient and its Faces. Face::read
// touches ONLY this block, so a Face that outlives the client stays safe. The
// JSONL log sink lives here (not on PolicyClient) for the same reason: a
// surviving Face can still emit tick lines, and the ofstream is closed only
// when the last owner — client or Face — drops. All scalars are atomic because
// the inference thread writes them while Face threads read them.
struct PolicyClient::SharedState {
  ChunkSlot chunk_slot;
  std::atomic<int> total_n{0};
  std::atomic<double> control_rate_hz{30.0};
  std::atomic<double> output_ema_alpha{1.0};
  std::atomic<double> output_ema_alpha_gripper{1.0};

  // JSONL log sink, shared with the owning client's request/response logging.
  // The mutex serializes line writes so multi-arm setups don't interleave
  // bytes mid-line.
  std::ofstream log_file;
  std::mutex log_mu;
  std::chrono::steady_clock::time_point log_t0{};

  /// Append one per-tick action line. Best-effort; a no-op when no log file is
  /// open. Safe to call from any Face thread and after the owning client is
  /// gone (the sink lives as long as this block). Defined out-of-line below,
  /// after the timestamp helper it depends on.
  void log_tick(const std::string& face_id,
                std::chrono::steady_clock::time_point t,
                const std::vector<float>& action,
                const ChunkSlot::SampleInfo* info_ptr) noexcept;
};

PolicyClient::PolicyClient(std::string id)
  : HardwareComponent(id),
    ObserverBase(id),
    shared_(std::make_shared<SharedState>()) {}

PolicyClient::PolicyClient(configuration::PolicyClientConfig cfg,
                           std::unique_ptr<PolicyTransport> transport)
  : HardwareComponent(cfg.id),
    ObserverBase(cfg.id),
    shared_(std::make_shared<SharedState>()) {
  if (!transport) {
    throw std::invalid_argument(
      "PolicyClient: transport must not be null for policy_client '" + cfg.id + "'");
  }
  init_(std::move(cfg), std::move(transport));
}

PolicyClient::~PolicyClient() {
  // Order: stop the inference thread + observer worker first so no live thread
  // touches transport_ during teardown, then drop each Face from the registry
  // so external lookups cannot resolve to a soon-dead object. Faces themselves
  // stay safe regardless of order: they hold the shared state, not the client,
  // so any Face retained elsewhere degrades to hold-last after we are gone.
  try {
    stop();
  } catch (...) {
    // stop() already swallows derived hook exceptions; nothing useful to do here.
  }
  for (const auto& face : faces_) {
    ActiveHardwareRegistry::unregister(face->get_identifier());
  }
}

void PolicyClient::configure(const nlohmann::json& cfg) {
  if (configured_) {
    return;
  }
  auto parsed = configuration::PolicyClientConfig::from_json(cfg);
  // Back-compat shim: top-level "api_key" predates transport_config. Inject
  // it so factories read a single place; an explicit transport_config entry
  // wins (the more specific setting beats the more general one).
  nlohmann::json transport_config = parsed.transport_config;
  if (parsed.api_key.has_value() && !transport_config.contains("api_key")) {
    transport_config["api_key"] = *parsed.api_key;
  }
  // Inject the flattened per-motor keys ("<joint_name>.pos", layout order) so a
  // name-keyed transport (LeRobot) has a single source of truth for the
  // observation.state component names — the same names map_observation_ stamps
  // on the per-step observation. Skipped when joint_names are unset (the
  // transport then requires explicit feature names or, like openpi, ignores it).
  // An explicit transport_config entry wins.
  if (!transport_config.contains("motor_names")) {
    nlohmann::json motor_names = nlohmann::json::array();
    for (const auto& row : parsed.joint_layout) {
      for (const auto& jn : row.joint_names) {
        motor_names.push_back(jn + ".pos");
      }
    }
    if (!motor_names.empty()) {
      transport_config["motor_names"] = std::move(motor_names);
    }
  }
  auto transport = TransportRegistry::create(
    parsed.transport, parsed.id, parsed.server_url, transport_config);
  init_(std::move(parsed), std::move(transport));
}

void PolicyClient::init_(configuration::PolicyClientConfig cfg,
                         std::unique_ptr<PolicyTransport> transport) {
  cfg_ = std::move(cfg);
  transport_ = std::move(transport);

  // inference_hz drives the loop period as 1e9 / inference_hz; a zero, negative,
  // or non-finite value would make the period UB or run the schedule backward.
  // The from_json path validates this, but the direct (two-arg) constructor and
  // programmatic callers do not go through it, so guard here as well.
  if (!(cfg_.inference_hz > 0.0) || !std::isfinite(cfg_.inference_hz)) {
    throw std::runtime_error(
      "PolicyClient: inference_hz must be finite and > 0 (got " +
      std::to_string(cfg_.inference_hz) + ") for '" + cfg_.id + "'");
  }

  // Partition subscriptions by obs_key prefix and validate openpi conventions.
  for (const auto& sub : cfg_.subscriptions) {
    if (sub.obs_key.rfind(kObsPrefixState, 0) == 0) {
      state_subs_.push_back(&sub);
      continue;
    }
    if (sub.obs_key.rfind(kObsPrefixImages, 0) == 0) {
      const std::string cam_key = sub.obs_key.substr(std::strlen(kObsPrefixImages));
      if (!is_expected_camera(cam_key)) {
        throw std::runtime_error(
          "PolicyClient: obs_key '" + sub.obs_key +
          "' references unsupported camera '" + cam_key +
          "'. EXPECTED_CAMERAS = {cam_high, cam_low, cam_left_wrist, cam_right_wrist}");
      }
      image_subs_.push_back(&sub);
      image_camera_keys_.push_back(cam_key);
      continue;
    }
    throw std::runtime_error(
      "PolicyClient: obs_key '" + sub.obs_key +
      "' must start with 'state.' or 'images.'");
  }

  if (state_subs_.size() != cfg_.joint_layout.size()) {
    throw std::runtime_error(
      "PolicyClient: number of state.* subscriptions (" +
      std::to_string(state_subs_.size()) +
      ") must equal joint_layout entries (" +
      std::to_string(cfg_.joint_layout.size()) + ")");
  }

  for (const auto& sub : cfg_.subscriptions) {
    const std::string record_id = sub.record_id;
    // Initialize the generation counter to 0 so the freshness barrier's
    // baseline lookup never falls through to an inserted-default and so the
    // first wait reduces to "every subscription has delivered at least once"
    // (the legacy prime behavior).
    cache_gen_[record_id] = 0;
    add_subscription(
      record_id, sub.throttle_hz,
      [this, record_id](const std::shared_ptr<data::RecordBase>& rec) {
        {
          std::lock_guard<std::mutex> lk(cache_mu_);
          latest_records_[record_id] = rec;
          ++cache_gen_[record_id];
        }
        // notify_all without holding cache_mu_ keeps the producer hot path
        // short and matches the existing observer notify_one pattern.
        cache_fresh_cv_.notify_all();
      });
  }

  // All-or-nothing Face registration: detect collisions first so an unrecoverable
  // partial state cannot leak into the registry, then register with an RAII
  // rollback guard in case a later step throws.
  std::vector<std::shared_ptr<Face>> staged;
  staged.reserve(cfg_.joint_layout.size());
  for (const auto& row : cfg_.joint_layout) {
    if (ActiveHardwareRegistry::is_registered(row.leader_id)) {
      throw std::runtime_error(
        "PolicyClient: leader_id '" + row.leader_id +
        "' is already registered in ActiveHardwareRegistry");
    }
    staged.push_back(std::make_shared<Face>(
      shared_, row.leader_id, row.joint_offset, row.joint_count));
  }

  std::vector<std::string> registered_ids;
  registered_ids.reserve(staged.size());
  auto rollback = [&]() noexcept {
    for (const auto& id : registered_ids) {
      ActiveHardwareRegistry::unregister(id);
    }
  };
  try {
    for (std::size_t i = 0; i < staged.size(); ++i) {
      ActiveHardwareRegistry::register_active(
        cfg_.joint_layout[i].leader_id, staged[i]);
      registered_ids.push_back(cfg_.joint_layout[i].leader_id);
    }
    faces_ = std::move(staged);
    int total = 0;
    for (const auto& row : cfg_.joint_layout) {
      total += row.joint_count;
    }
    shared_->total_n.store(total, std::memory_order_release);
  } catch (...) {
    rollback();
    throw;
  }

  // Publish the output-filter coefficients into the shared block so Faces read
  // them without touching the client (they are fixed for the object's life).
  shared_->output_ema_alpha.store(cfg_.output_ema_alpha, std::memory_order_release);
  shared_->output_ema_alpha_gripper.store(
    cfg_.output_ema_alpha_gripper, std::memory_order_release);

  open_log_file_(cfg_.log_path);

  shared_->chunk_slot.set_boundary_blend_s(cfg_.chunk_boundary_blend_s);

  // Tell the slot to skip the boundary cross-fade on gripper channels. By
  // joint_layout convention each entry's last column is the gripper; with
  // bimanual ALOHA that's indices 6 and 13. The blend exists to smooth arm
  // motion across chunk promotions, but gripper transitions are fast and
  // commanding-the-old-value-during-blend produces incomplete grasps.
  std::vector<int> gripper_indices;
  gripper_indices.reserve(cfg_.joint_layout.size());
  for (const auto& row : cfg_.joint_layout) {
    if (row.joint_count > 0) {
      gripper_indices.push_back(row.joint_offset + row.joint_count - 1);
    }
  }
  shared_->chunk_slot.set_boundary_blend_skip_indices(std::move(gripper_indices));

  configured_ = true;
}

void PolicyClient::set_inference_active(bool active) noexcept {
  const bool was_active = inference_active_.exchange(active, std::memory_order_acq_rel);
  if (was_active && !active) {
    // Going active → paused: clear the chunk slot so a stale chunk does not
    // drive the mirror after resume. Faces fall back to hold-last-action,
    // which keeps the follower at its last commanded pose during reset.
    // (ChunkSlot is internally synchronized, so this cross-thread call is safe.)
    shared_->chunk_slot.swap_in(nullptr);
    // The chunk-exhaust/fire wait targets are NOT touched here: they are owned
    // by the inference thread, which clears them when it wakes from the pause
    // (see inference_loop_). Writing them from this thread would race the loop.
    // Reset each Face's EMA filter history so the first read after resume
    // doesn't blend the new episode's chunks against pre-pause output.
    for (const auto& face : faces_) {
      if (face) face->reset_output_filter();
    }
  }
  inference_cv_.notify_all();
  // Unblock any in-flight freshness wait so a pause/resume takes effect
  // immediately rather than after the freshness_timeout_ms deadline.
  cache_fresh_cv_.notify_all();
}

void PolicyClient::set_control_rate_hz(double hz) noexcept {
  if (hz > 0.0) {
    shared_->control_rate_hz.store(hz, std::memory_order_release);
  }
}

double PolicyClient::control_rate_hz() const noexcept {
  return shared_->control_rate_hz.load(std::memory_order_acquire);
}

int PolicyClient::total_joint_count() const noexcept {
  return shared_->total_n.load(std::memory_order_acquire);
}

std::shared_ptr<const ActionChunk> PolicyClient::latest_chunk() const noexcept {
  return shared_->chunk_slot.peek();
}

std::vector<float> PolicyClient::current_command() const {
  return shared_->chunk_slot.sample(
    std::chrono::steady_clock::now(),
    shared_->total_n.load(std::memory_order_acquire),
    shared_->control_rate_hz.load(std::memory_order_acquire));
}

bool PolicyClient::on_start() {
  if (!transport_) {
    std::cerr << "[policy_client:" << name()
              << "] on_start invoked without a configured transport\n";
    return false;
  }
  try {
    transport_->connect();
  } catch (const std::exception& e) {
    std::cerr << "[policy_client:" << name() << "] transport connect failed: "
              << e.what() << "\n";
    return false;
  } catch (...) {
    std::cerr << "[policy_client:" << name() << "] transport connect failed (unknown)\n";
    return false;
  }

  inference_running_.store(true, std::memory_order_release);
  try {
    inference_thread_ = std::thread(&PolicyClient::inference_loop_, this);
  } catch (...) {
    inference_running_.store(false, std::memory_order_release);
    transport_->close();
    std::cerr << "[policy_client:" << name() << "] failed to spawn inference thread\n";
    return false;
  }
  return true;
}

void PolicyClient::on_stop() {
  inference_running_.store(false, std::memory_order_release);
  inference_cv_.notify_all();
  // Also wake any freshness wait so shutdown does not block for up to
  // freshness_timeout_ms.
  cache_fresh_cv_.notify_all();
  // Transport close() joins its own worker (unblocking any in-flight round
  // trip); the loop's chunk-poll wait exits via inference_running_ + the cv
  // notifies above. Order is free now — nothing here can block on the server.
  if (transport_) {
    transport_->close();
  }
  if (inference_thread_.joinable()) {
    inference_thread_.join();
  }
}

void PolicyClient::inference_loop_() {
  using clock = std::chrono::steady_clock;
  const double inference_hz = cfg_.inference_hz;
  const auto period = std::chrono::nanoseconds(
    static_cast<int64_t>(1e9 / inference_hz));

  uint64_t req_seq = 0;
  auto next = clock::now();
  // Timestep Clock origin for the initial active window (re-set on resume).
  inference_epoch_ = next;
  // Wall-clock timestamp of the previous successful packing. Used to populate
  // cycle_interval_ms; zero on the first cycle of an active window. Reset to
  // zero whenever the loop pauses so a pause/resume doesn't masquerade as a
  // very long cycle interval.
  std::chrono::steady_clock::time_point prev_pack_time{};

  // Transport health is logged on TRANSITIONS only — a disconnect mid-episode
  // is one warning line, not a per-cycle error stream. kConnected start state
  // is correct by construction: on_start() connected before spawning us.
  auto prev_transport_state = TransportStatus::State::kConnected;
  auto state_name = [](TransportStatus::State s) {
    switch (s) {
      case TransportStatus::State::kConnected: return "connected";
      case TransportStatus::State::kReconnecting: return "reconnecting";
      case TransportStatus::State::kDisconnected: return "disconnected";
    }
    return "unknown";
  };

  while (inference_running_.load(std::memory_order_acquire)) {
    // Block while paused. Skip work, do not consume the period budget — the
    // loop should resume in real time, not catch up on missed cycles.
    if (!inference_active_.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> lk(inference_mu_);
      inference_cv_.wait(lk, [this] {
        return !inference_running_.load(std::memory_order_acquire) ||
               inference_active_.load(std::memory_order_acquire);
      });
      if (!inference_running_.load(std::memory_order_acquire)) break;
      // Reset the schedule clock so we do not fire a burst of catch-up
      // requests immediately after resume. Drop the previous pack time
      // so the first cycle after resume reports cycle_interval_ms=0.
      lk.unlock();
      next = clock::now();
      prev_pack_time = {};
      // Reset the Timestep Clock epoch: post-resume observations restart at
      // tick 0, so anything packed before the pause is stale by construction.
      inference_epoch_ = next;
      // Clear the chunk-exhaust/fire wait targets here, on the inference
      // thread, rather than from set_inference_active() on the caller's thread:
      // these members are read by wait_for_fire_point_/pack_observation_/
      // apply_chunk_ (all inference-thread), so keeping every write on this
      // thread makes them genuinely single-threaded and race-free. A stale
      // target from the prior episode would otherwise gate the first cycle on
      // an instant that has already passed (or is far in the future).
      next_chunk_exhaust_target_ = {};
      next_chunk_fire_target_ = {};
    }

    // One status poll per cycle; log transitions only (a disconnect mid-episode
    // is one warning line, not a per-cycle error stream).
    {
      const TransportStatus st = transport_->status();
      if (st.state != prev_transport_state) {
        std::cerr << "[policy_client:" << name() << "] transport "
                  << state_name(prev_transport_state) << " -> "
                  << state_name(st.state)
                  << " (failures=" << st.failure_count
                  << (st.last_error.empty() ? std::string()
                                            : ", last_error: " + st.last_error)
                  << ")\n";
        prev_transport_state = st.state;
      }
    }

    // Gate observation packing on the drain-threshold firing instant. At θ=0
    // this is chunk exhaustion (openpi semantics): packing waits until the
    // current chunk is about to exhaust so the observation captures the arm at
    // its end-of-chunk pose, avoiding the mid-chunk wall-clock schedule that
    // made the policy return chunks computed for a stale pose. At θ>0 it fires
    // earlier, overlapping inference with playback. Skipped on the first cycle
    // (no chunk yet) and after a pause-driven slot clear.
    wait_for_fire_point_();
    if (!inference_running_.load(std::memory_order_acquire)) break;
    if (!inference_active_.load(std::memory_order_acquire)) continue;

    // Wait for a fresh delivery from every subscription before packing.
    // Subsumes the legacy first-arrival prime: at startup/resume the
    // generation counters are all zero and this reduces to "wait for one
    // record per sub". Steady state: ensures every observation snapshot is
    // co-temporal (all records produced within one producer period).
    ObservationDiagnostics diag;
    diag.freshness_wait_ms = wait_for_fresh_observations_();
    // Pause / shutdown may have been requested while we were waiting.
    if (!inference_running_.load(std::memory_order_acquire)) break;
    if (!inference_active_.load(std::memory_order_acquire)) continue;

    std::optional<Observation> obs;
    try {
      obs = pack_observation_(diag);
    } catch (const std::exception& e) {
      warn_once_("pack_observation",
                 std::string("pack_observation threw: ") + e.what());
    } catch (...) {
      warn_once_("pack_observation", "pack_observation threw (unknown)");
    }

    if (obs) {
      ++req_seq;
      const auto t_send = clock::now();
      // Populate cycle_interval before logging; zero on the very first cycle
      // or first cycle after resume (prev_pack_time was reset).
      if (prev_pack_time.time_since_epoch().count() != 0) {
        diag.cycle_interval_ms =
          std::chrono::duration<double, std::milli>(t_send - prev_pack_time)
            .count();
      }
      prev_pack_time = t_send;
      log_request_(req_seq, t_send, *obs, diag);
      try {
        std::ostringstream line;
        line << "[policy_client:" << name() << "] req#" << req_seq << " state=[";
        line << std::fixed << std::setprecision(3);
        bool first = true;
        for (const auto& group : obs->state) {
          for (float v : group.values) {
            line << (first ? "" : ", ") << v;
            first = false;
          }
        }
        line << "] images=[";
        std::size_t k = 0;
        for (const auto& img : obs->images) {
          if (k++) line << ", ";
          line << img.camera
               << "(3x" << img.height << "x" << img.width << ")";
        }
        line << "] prompt=\"" << obs->task << "\"\n";
        std::cerr << line.str();
      } catch (...) {
        // Logging is best-effort; do not propagate.
      }

      transport_->push_observation(*obs);

      // Poll for the reply chunk (θ=0 cadence: the cycle re-synchronizes on
      // arrival, exactly like the old blocking round_trip). The 1 ms tick is
      // noise against ~1 s inference, and using inference_cv_ keeps the wait
      // shutdown-interruptible. Exits: chunk, shutdown, or transport gone
      // unhealthy (a disconnected transport polls nullopt forever). NOT on
      // pause — a chunk completing mid-pause is still applied, as before.
      // Inference deadline: a half-open server can stay kConnected yet never
      // return a chunk. Without a bound the loop would spin here forever,
      // silently sending no further observations while the arm holds last. On
      // the deadline we abandon this round-trip and fall through to the next
      // cycle, which re-packs and re-pushes a fresh observation (latest-wins
      // supersedes the abandoned one). cfg_.inference_timeout_ms <= 0 disables
      // the bound. Logged per-occurrence (ONGOING, not warn_once_) so a
      // persistent stall keeps surfacing.
      const bool deadline_enabled = (cfg_.inference_timeout_ms > 0.0);
      const auto deadline_dur = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double, std::milli>(cfg_.inference_timeout_ms));
      std::optional<ActionChunk> chunk;
      while (inference_running_.load(std::memory_order_acquire)) {
        chunk = transport_->try_poll_chunk();
        if (chunk) break;
        if (transport_->status().state !=
            TransportStatus::State::kConnected) {
          break;  // next cycle's status poll logs the transition
        }
        if (deadline_enabled && (clock::now() - t_send) > deadline_dur) {
          const double waited_ms =
            std::chrono::duration<double, std::milli>(clock::now() - t_send)
              .count();
          std::cerr << "[policy_client:" << name() << "] req#" << req_seq
                    << " no chunk after " << std::fixed << std::setprecision(0)
                    << waited_ms << " ms (deadline "
                    << cfg_.inference_timeout_ms
                    << " ms); abandoning round-trip, re-observing next cycle\n";
          break;  // abandon; the next cycle re-observes and re-pushes
        }
        std::unique_lock<std::mutex> lk(inference_mu_);
        inference_cv_.wait_for(lk, std::chrono::milliseconds(1), [this] {
          return !inference_running_.load(std::memory_order_acquire);
        });
      }

      if (chunk) {
        const auto t_recv = clock::now();
        // Push-to-arrival latency: true round trip plus up to one poll tick.
        // Same field name as the old blocking measurement (paired logs).
        const auto rt_ms =
          std::chrono::duration<double, std::milli>(t_recv - t_send).count();
        std::cerr << "[policy_client:" << name() << "] req#" << req_seq
                  << " round_trip=" << std::fixed << std::setprecision(1)
                  << rt_ms << " ms\n";
        apply_chunk_(std::move(*chunk), req_seq, rt_ms);
      }
    }

    next += period;
    std::unique_lock<std::mutex> lk(inference_mu_);
    inference_cv_.wait_until(lk, next, [this] {
      return !inference_running_.load(std::memory_order_acquire);
    });
  }
}

int64_t PolicyClient::current_timestep_(
    std::chrono::steady_clock::time_point now) const noexcept {
  const auto epoch = inference_epoch_;
  if (epoch.time_since_epoch().count() == 0) {
    return 0;  // no active window yet
  }
  const double rate = shared_->control_rate_hz.load(std::memory_order_acquire);
  if (!(rate > 0.0)) {
    return 0;  // control rate unknown — the clock cannot advance
  }
  const double secs = std::chrono::duration<double>(now - epoch).count();
  if (secs <= 0.0) {
    return 0;
  }
  return static_cast<int64_t>(std::floor(secs * rate));
}

void PolicyClient::wait_for_fire_point_() {
  // Read the target captured by the previous cycle's apply_chunk_. Tracking
  // it here (rather than asking the slot via exhaustion_time) is deliberate:
  // chunk N+1 lands in the slot's pending_ slot, and the slot's latest_ is
  // only swapped to N+1 by the next face sample(), which happens up to one
  // face period later. Reading the slot at the top of cycle N+2 therefore
  // sees chunk N's already-expired time and the wait short-circuits. We
  // instead wait for the *applied* chunk's firing instant (θ point).
  const auto target = next_chunk_fire_target_;
  // Default-constructed (epoch zero) signals "no chunk applied yet" — first
  // cycle of an active window or just after a pause clear. Proceed
  // immediately; the freshness barrier still holds us off until a record
  // per subscription is in the cache.
  if (target.time_since_epoch().count() == 0) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (target <= now) {
    // Already exhausted (e.g. an inference round-trip was longer than the
    // chunk's playback duration). Pack right away.
    return;
  }
  std::unique_lock<std::mutex> lk(inference_mu_);
  inference_cv_.wait_until(lk, target, [this] {
    // Wake immediately on pause/shutdown. Chunk delivery and observation
    // arrivals don't signal inference_cv_, so they can't false-wake us.
    return !inference_running_.load(std::memory_order_acquire) ||
           !inference_active_.load(std::memory_order_acquire);
  });
}

double PolicyClient::wait_for_fresh_observations_() {
  using clock = std::chrono::steady_clock;
  const auto start = clock::now();
  // RAII-style timer that always reports the wait duration in ms, regardless
  // of how the function exits (success, timeout, or shutdown race).
  auto elapsed_ms = [&]() {
    return std::chrono::duration<double, std::milli>(clock::now() - start)
      .count();
  };

  // Snapshot baseline generation per subscription, then block on the CV until
  // every counter has advanced. Implements the openpi-equivalent of clearing
  // each camera's new_frame_event before issuing async_read.
  std::unordered_map<std::string, uint64_t> baseline;
  baseline.reserve(cfg_.subscriptions.size());

  std::unique_lock<std::mutex> lk(cache_mu_);
  for (const auto& sub : cfg_.subscriptions) {
    // cache_gen_[id] was initialized to 0 in init_(); the map already
    // contains every subscription key. Use at() to assert that invariant
    // under hardened builds.
    baseline[sub.record_id] = cache_gen_.at(sub.record_id);
  }

  // Distinguish "every sub has delivered at least once since baseline" (true)
  // from "shutdown / pause requested" (also true; outer loop reacts) for the
  // post-wait diagnostic.
  const bool first_wait = !primed_observation_cache_;
  if (first_wait) {
    std::cerr << "[policy_client:" << name()
              << "] waiting for first observation from "
              << cfg_.subscriptions.size() << " subscription(s)...\n";
  }

  const auto timeout = std::chrono::milliseconds(
    static_cast<int64_t>(cfg_.freshness_timeout_ms));

  auto all_advanced = [&]() {
    for (const auto& sub : cfg_.subscriptions) {
      if (cache_gen_.at(sub.record_id) <= baseline.at(sub.record_id)) {
        return false;
      }
    }
    return true;
  };

  const bool advanced = cache_fresh_cv_.wait_for(lk, timeout, [&]() {
    if (!inference_running_.load(std::memory_order_acquire)) return true;
    if (!inference_active_.load(std::memory_order_acquire)) return true;
    return all_advanced();
  });

  if (!inference_running_.load(std::memory_order_acquire) ||
      !inference_active_.load(std::memory_order_acquire)) {
    // Shutdown / pause raced the wait; outer loop handles it.
    return elapsed_ms();
  }

  if (advanced && all_advanced()) {
    if (first_wait) {
      std::cerr << "[policy_client:" << name()
                << "] observation cache primed in "
                << std::fixed << std::setprecision(2)
                << elapsed_ms() / 1000.0 << "s\n";
      primed_observation_cache_ = true;
    }
    return elapsed_ms();
  }

  // Timeout. Identify stale subscriptions for the one-shot warning.
  std::vector<std::string> stale;
  for (const auto& sub : cfg_.subscriptions) {
    if (cache_gen_.at(sub.record_id) <= baseline.at(sub.record_id)) {
      stale.push_back(sub.record_id);
    }
  }
  lk.unlock();  // release before formatting / logging

  std::ostringstream oss;
  oss << "freshness barrier timed out after "
      << std::fixed << std::setprecision(0) << cfg_.freshness_timeout_ms
      << "ms; proceeding with stale snapshot for: ";
  for (std::size_t i = 0; i < stale.size(); ++i) {
    oss << stale[i] << (i + 1 < stale.size() ? ", " : "");
  }
  warn_once_("freshness_timeout", oss.str());
  // Latch primed_ even on first-wait timeout so the "waiting for first
  // observation" banner doesn't repeat each cycle when a source is stuck.
  primed_observation_cache_ = true;
  return elapsed_ms();
}

std::optional<Observation> PolicyClient::pack_observation_(
    ObservationDiagnostics& diag) {
  std::unordered_map<std::string, std::shared_ptr<data::RecordBase>> snapshot;
  {
    std::lock_guard<std::mutex> lk(cache_mu_);
    snapshot = latest_records_;
  }

  // Compute per-record age relative to packing instant. Records carry their
  // monotonic capture timestamp; subtract from steady_clock::now() to get age
  // in ms. NaN signals "record missing from cache" so the consumer can tell
  // missing from fresh.
  {
    const uint64_t now_ns =
      static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
    constexpr double kNanToMilli = 1.0 / 1e6;
    double min_age = std::numeric_limits<double>::infinity();
    double max_age = -std::numeric_limits<double>::infinity();
    bool any_age = false;
    diag.ages_ms.clear();
    diag.ages_ms.reserve(cfg_.subscriptions.size());
    for (const auto& sub : cfg_.subscriptions) {
      auto it = snapshot.find(sub.record_id);
      if (it == snapshot.end() || !it->second) {
        diag.ages_ms[sub.record_id] = std::numeric_limits<double>::quiet_NaN();
        continue;
      }
      const uint64_t rec_ns = it->second->ts.monotonic.to_ns();
      const double age_ms =
        rec_ns <= now_ns ? static_cast<double>(now_ns - rec_ns) * kNanToMilli
                         : 0.0;  // clock skew guard (rec stamped after now)
      diag.ages_ms[sub.record_id] = age_ms;
      if (age_ms < min_age) min_age = age_ms;
      if (age_ms > max_age) max_age = age_ms;
      any_age = true;
    }
    diag.skew_ms = any_age ? max_age - min_age : 0.0;
  }

  // state: one named group per joint_layout entry, layout order. Grouping is
  // preserved (not flattened) because flattening is lossy: LeRobot's rename
  // map needs group identity; openpi re-flattens inside its transport.
  Observation obs;
  obs.captured_at = std::chrono::steady_clock::now();
  obs.task = cfg_.prompt;
  // Timestep Clock: control-rate ticks since this active window's epoch. Pairs
  // with the returned chunk's base_timestep for aligned take-over.
  obs.timestep = current_timestep_(obs.captured_at);
  // must_go: the action buffer is empty — no chunk applied yet, or the last
  // applied chunk has played past its final row by now. The server prioritizes
  // such observations. At θ>0 the fire point precedes exhaustion (overlap), so
  // must_go marks the exceptional stall/slow-server case; at θ=0 the buffer
  // empties every cycle by design, so it is set each time.
  obs.must_go = (next_chunk_exhaust_target_.time_since_epoch().count() == 0) ||
                (obs.captured_at >= next_chunk_exhaust_target_);

  obs.state.reserve(state_subs_.size());
  for (std::size_t i = 0; i < state_subs_.size(); ++i) {
    const auto* sub = state_subs_[i];
    const int joint_count = cfg_.joint_layout[i].joint_count;
    Observation::StateGroup group;
    group.name = cfg_.joint_layout[i].leader_id;
    // Per-joint motor names from config (may be empty). Transports that key
    // state by name (LeRobot's "<name>.pos") use them; openpi ignores them.
    group.joint_names = cfg_.joint_layout[i].joint_names;
    auto it = snapshot.find(sub->record_id);
    std::shared_ptr<data::RecordBase> rec =
      (it != snapshot.end()) ? it->second : nullptr;
    auto js = rec
      ? std::dynamic_pointer_cast<data::JointStateRecord>(rec)
      : nullptr;
    if (!js) {
      if (!rec) {
        warn_once_("missing_record:" + sub->record_id,
                   "no record yet for '" + sub->record_id +
                   "', zero-filling state slice");
      } else {
        warn_once_("unsupported_record:" + sub->record_id,
                   "record '" + sub->record_id +
                   "' is not a JointStateRecord; zero-filling state slice");
      }
      group.values.assign(static_cast<std::size_t>(joint_count), 0.0f);
    } else {
      group.values = js->positions;
      // Pad-or-truncate to the layout's declared width (resize zero-fills).
      group.values.resize(static_cast<std::size_t>(joint_count), 0.0f);
    }
    obs.state.push_back(std::move(group));
  }

  // Images: one entry per configured camera, ALWAYS in image_subs_ order and
  // count. pack_image_ handles a null/bad/unmappable record by substituting a
  // same-shaped zero frame (see F7), so the observation's image set never
  // depends on which cameras happened to deliver this cycle. A std::nullopt
  // means a configured camera cannot be represented at all yet (no valid frame
  // and no dimensions to synthesize one); publishing an observation with a
  // missing camera would silently change its shape, so fail the whole cycle.
  obs.images.reserve(image_subs_.size());
  for (std::size_t i = 0; i < image_subs_.size(); ++i) {
    const auto* sub = image_subs_[i];
    const std::string& cam_key = image_camera_keys_[i];
    auto it = snapshot.find(sub->record_id);
    std::shared_ptr<data::RecordBase> rec =
      (it != snapshot.end()) ? it->second : nullptr;
    auto img = pack_image_(rec, *sub, cam_key);
    if (!img) {
      warn_once_("camera_unrepresentable:" + cam_key,
                 "cannot represent configured camera '" + cam_key +
                 "' (no valid frame and no resize/last-known dimensions to "
                 "synthesize a placeholder); skipping this observation");
      return std::nullopt;
    }
    obs.images.push_back(std::move(*img));
  }

  return obs;
}

std::optional<Observation::Image> PolicyClient::pack_image_(
    const std::shared_ptr<data::RecordBase>& rec,
    const configuration::PolicyClientSubscriptionConfig& sub,
    const std::string& camera_key) {
  auto img_ptr = rec ? std::dynamic_pointer_cast<data::ImageRecord>(rec)
                     : nullptr;
  if (!img_ptr || img_ptr->image.empty()) {
    // Missing / wrong-type / empty record. Substitute a same-shaped zero frame
    // so the observation's image set stays stable regardless of which cameras
    // delivered this cycle (see F7).
    warn_once_(
      "missing_image:" + sub.record_id,
      rec ? ("record '" + sub.record_id +
             "' is not a non-empty ImageRecord; substituting zero frame for "
             "camera '" + camera_key + "'")
          : ("no image record yet for '" + sub.record_id +
             "'; substituting zero frame for camera '" + camera_key + "'"));
    return substitute_image_(sub, camera_key);
  }
  const auto& img = *img_ptr;

  int target_w = img.image.cols;
  int target_h = img.image.rows;
  if (sub.resize.has_value()) {
    target_w = sub.resize->first;
    target_h = sub.resize->second;
  }

  cv::Mat resized;
  if (target_w != img.image.cols || target_h != img.image.rows) {
    cv::resize(img.image, resized, cv::Size(target_w, target_h));
  } else {
    resized = img.image;
  }

  // The neutral Observation contract is TRUE RGB, always. Derive the channel
  // count from the actual Mat, NOT the encoding string: a mono8 (1-channel)
  // frame has cols bytes/row, so copying cols*3 bytes/row would over-read the
  // heap. mono8 is expanded to RGB; bgr8 is converted; rgb8 (and any other
  // 3-channel frame) passes through. A channel count that cannot map to RGB
  // is rejected via the same zero-substitute path as a missing frame. Any
  // model-specific channel quirk (openpi's BGR training convention) is applied
  // inside that transport, never here.
  cv::Mat rgb;
  const int channels = resized.channels();
  if (channels == 1) {
    cv::cvtColor(resized, rgb, cv::COLOR_GRAY2RGB);
  } else if (channels == 3) {
    if (img.encoding == "bgr8") {
      cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
    } else {
      rgb = resized;  // rgb8 (camera-native) passes through
    }
  } else {
    warn_once_("unsupported_image_channels:" + sub.record_id,
               "record '" + sub.record_id + "' image has " +
               std::to_string(channels) +
               " channels, which cannot map to RGB; substituting zero frame "
               "for camera '" + camera_key + "'");
    return substitute_image_(sub, camera_key);
  }

  Observation::Image out;
  out.camera = camera_key;
  out.width = rgb.cols;
  out.height = rgb.rows;
  // rgb now has exactly 3 channels, so cols*3 bytes/row is valid.
  const std::size_t row_bytes = static_cast<std::size_t>(rgb.cols) * 3;
  out.rgb.resize(static_cast<std::size_t>(rgb.rows) * row_bytes);
  // Row-wise copy: cv::Mat rows may be padded (non-continuous), e.g. for
  // views; HWC interleaved layout is shared by cv::Mat and the contract.
  for (int y = 0; y < rgb.rows; ++y) {
    std::memcpy(out.rgb.data() + static_cast<std::size_t>(y) * row_bytes,
                rgb.ptr<uint8_t>(y), row_bytes);
  }
  // Remember the shape so a later missing/unmappable frame for this camera can
  // still be substituted with a correctly-sized zero frame even when no resize
  // is configured. Inference-thread only, so no lock needed.
  last_image_dims_[camera_key] = {out.width, out.height};
  return out;
}

std::optional<Observation::Image> PolicyClient::substitute_image_(
    const configuration::PolicyClientSubscriptionConfig& sub,
    const std::string& camera_key) {
  int w = 0;
  int h = 0;
  if (sub.resize.has_value()) {
    w = sub.resize->first;
    h = sub.resize->second;
  } else {
    auto it = last_image_dims_.find(camera_key);
    if (it != last_image_dims_.end()) {
      w = it->second.first;
      h = it->second.second;
    }
  }
  if (w > 0 && h > 0) {
    return zero_image_(camera_key, w, h);
  }
  // No resize and no prior frame: we cannot size a placeholder. The caller
  // fails the whole observation rather than shipping a diverging image set.
  return std::nullopt;
}

Observation::Image PolicyClient::zero_image_(
    const std::string& camera_key, int width, int height) const {
  Observation::Image img;
  img.camera = camera_key;
  img.width = width;
  img.height = height;
  img.rgb.assign(static_cast<std::size_t>(width) *
                 static_cast<std::size_t>(height) * 3,
                 uint8_t{0});
  return img;
}

void PolicyClient::apply_chunk_(ActionChunk chunk_in,
                                uint64_t request_seq, double rt_ms) {
  // Wire validation (shape/dtype/size) happened in the transport's decoder,
  // which also stamped chunk_seq (per-connection) and received_at. The
  // client's own gate is the one fact the transport cannot know: the chunk
  // width must match the configured joint layout.
  const int total_n = shared_->total_n.load(std::memory_order_acquire);
  if (chunk_in.N != total_n) {
    warn_once_("chunk_n_mismatch",
               "policy reply actions N=" + std::to_string(chunk_in.N) +
               " does not match sum(joint_count)=" + std::to_string(total_n) +
               "; ignoring chunk (hold-last-action)");
    chunks_rejected_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Safety gate: this chunk is about to drive physical arms. A single
  // non-finite action (NaN/Inf from a diverged or malfunctioning server) would
  // command a garbage pose. Scan the whole [T x N] buffer and reject the WHOLE
  // chunk on the first non-finite element — never publish a partially
  // sanitized chunk. Rejection is hold-last-action: the slot keeps playing the
  // chunk it already has (or its last row). Throttled (not warn_once_) so a
  // persistently bad server keeps surfacing rather than warning only once.
  // NOTE: per-joint range/limit clamping is a fast-follow — this branch has no
  // per-joint bounds surface to check against yet, so only finiteness is gated.
  for (float v : chunk_in.data) {
    if (!std::isfinite(v)) {
      warn_throttled_("chunk_non_finite",
                      "policy reply chunk contains a non-finite action "
                      "(NaN/Inf); rejecting the whole chunk (hold-last-action)",
                      std::chrono::milliseconds(1000));
      chunks_rejected_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }

  auto chunk = std::make_shared<const ActionChunk>(std::move(chunk_in));

  try {
    std::ostringstream line;
    line << "[policy_client:" << name() << "] chunk#" << chunk->chunk_seq
         << " T=" << chunk->T << " N=" << chunk->N << " row[0]=["
         << std::fixed << std::setprecision(3);
    for (int i = 0; i < chunk->N; ++i) {
      line << chunk->data[i] << (i + 1 < chunk->N ? ", " : "");
    }
    line << "]\n";
    std::cerr << line.str();
  } catch (...) {
    // Logging is best-effort.
  }

  log_response_(request_seq, chunk->received_at, rt_ms, chunk);

  const double rate = shared_->control_rate_hz.load(std::memory_order_acquire);

  // Async-overlap path (drain threshold θ>0): the chunk was inferred while the
  // previous one still plays, so it takes over immediately, aligned to the
  // Timestep Clock. Row 0 anchors at epoch + base_timestep/rate; rows already
  // in the past are skipped by sample(), and an all-past chunk is discarded.
  if (cfg_.drain_threshold > 0.0 && rate > 0.0 && chunk->T > 0 &&
      inference_epoch_.time_since_epoch().count() != 0) {
    const auto now = std::chrono::steady_clock::now();
    const int64_t ts_now = current_timestep_(now);
    const int64_t start_row = ts_now - chunk->base_timestep;
    if (start_row >= chunk->T) {
      warn_once_("chunk_all_past",
                 "policy reply chunk is already fully in the past "
                 "(base_timestep behind the timestep clock); discarding");
      chunks_rejected_.fetch_add(1, std::memory_order_relaxed);
      return;  // hold-last-action; keep the chunk currently playing
    }
    // Symmetric forward bound: a far-future base_timestep (server clock ahead,
    // or a corrupt/garbage value) makes start_row strongly negative, so the
    // all-past guard above passes, yet aligned_start / the fire target land far
    // ahead — the arm would stall on hold-last for a long time. Reject any
    // chunk claiming to start more than a full chunk's worth of ticks ahead of
    // the timestep clock.
    if (chunk->base_timestep - ts_now > chunk->T) {
      warn_once_("chunk_far_future",
                 "policy reply chunk starts more than a full chunk ahead of "
                 "the timestep clock (base_timestep far in the future); "
                 "discarding (hold-last-action)");
      chunks_rejected_.fetch_add(1, std::memory_order_relaxed);
      return;  // hold-last-action; keep the chunk currently playing
    }
    const auto tick_ns = [rate](int64_t ticks) {
      return std::chrono::nanoseconds(
        static_cast<int64_t>(static_cast<double>(ticks) / rate * 1e9));
    };
    const auto aligned_start = inference_epoch_ + tick_ns(chunk->base_timestep);
    next_chunk_exhaust_target_ = aligned_start + tick_ns(chunk->T);
    // Fire the next observation when the remaining playable fraction drops to
    // θ: at (1-θ)·T ticks into this chunk.
    const double play_fraction = 1.0 - cfg_.drain_threshold;
    next_chunk_fire_target_ = aligned_start + std::chrono::nanoseconds(
      static_cast<int64_t>(play_fraction * static_cast<double>(chunk->T) /
                           rate * 1e9));
    shared_->chunk_slot.swap_in_aligned(chunk, aligned_start, now);
    chunks_published_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Consume-fully path (openpi θ=0 cadence). The two members set below are
  // scheduling ESTIMATES the client uses to time the next cycle's fire point;
  // they are not the slot's actual playback anchor. The slot itself parks this
  // chunk in its pending_ slot (when one is already playing) and, on the sample
  // tick where the current chunk exhausts (t_idx >= T), promotes it and
  // re-anchors playback_start_ = now (that sample instant). We compute the
  // estimate BEFORE moving the chunk into the slot — once swap_in runs we no
  // longer own the pointer — using max(received_at, previous exhaust estimate)
  // as the base so the next cycle's wait gates on roughly the right instant
  // whether or not the previous chunk was still playing when this one arrived.
  if (rate > 0.0 && chunk->T > 0) {
    const auto duration_ns = std::chrono::nanoseconds(
      static_cast<int64_t>(
        static_cast<double>(chunk->T) / rate * 1e9));
    const auto base = (next_chunk_exhaust_target_ > chunk->received_at)
      ? next_chunk_exhaust_target_   // previous chunk hadn't exhausted yet
      : chunk->received_at;           // previous chunk done; we play from now
    next_chunk_exhaust_target_ = base + duration_ns;
    // θ=0 on this path (θ>0 takes the aligned branch above), so the fire point
    // coincides with exhaustion. Compute it via the same θ rule for clarity.
    const double play_fraction = 1.0 - cfg_.drain_threshold;
    next_chunk_fire_target_ = base + std::chrono::nanoseconds(
      static_cast<int64_t>(play_fraction * static_cast<double>(chunk->T) /
                           rate * 1e9));
  }

  shared_->chunk_slot.swap_in(std::move(chunk));
  chunks_published_.fetch_add(1, std::memory_order_relaxed);
}

namespace {

// Expand a leading "~" or "~/..." to the value of $HOME. Other paths pass
// through unchanged.
std::filesystem::path expand_tilde(const std::string& path) {
  if (path.empty() || path[0] != '~') return path;
  const char* home = std::getenv("HOME");
  if (!home || *home == '\0') return path;
  if (path == "~") return home;
  if (path.size() >= 2 && path[1] == '/') {
    return std::filesystem::path(home) / path.substr(2);
  }
  return path;
}

// Render a steady_clock::time_point as seconds since steady epoch (relative,
// for inter-event timing) and a wall-clock ISO 8601 instant (absolute).
struct LogTimestamps {
  double mono_s;
  std::string wall_iso;
};
LogTimestamps render_timestamps(std::chrono::steady_clock::time_point t_mono,
                                std::chrono::steady_clock::time_point t0) {
  LogTimestamps ts;
  ts.mono_s = std::chrono::duration<double>(t_mono - t0).count();
  // Wall-clock approximation: take system_clock::now() at log time. This is
  // close enough for cross-process comparison (the openpi reference will
  // capture the same wall instant for its own line).
  const auto sys_now = std::chrono::system_clock::now();
  const auto sec = std::chrono::time_point_cast<std::chrono::seconds>(sys_now);
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                    sys_now - sec).count();
  std::time_t tt = std::chrono::system_clock::to_time_t(sec);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
      << '.' << std::setw(6) << std::setfill('0') << us << 'Z';
  ts.wall_iso = oss.str();
  return ts;
}

// Compute per-channel mean over HWC uint8 bytes (interleaved RGB layout).
std::vector<double> per_channel_mean_hwc(const uint8_t* bytes, int H, int W) {
  std::vector<double> out(3, 0.0);
  if (!bytes || H <= 0 || W <= 0) return out;
  const std::size_t pixels = static_cast<std::size_t>(H) *
                             static_cast<std::size_t>(W);
  uint64_t sums[3] = {0, 0, 0};
  for (std::size_t i = 0; i < pixels; ++i) {
    sums[0] += bytes[i * 3];
    sums[1] += bytes[i * 3 + 1];
    sums[2] += bytes[i * 3 + 2];
  }
  for (int c = 0; c < 3; ++c) {
    out[static_cast<std::size_t>(c)] =
      static_cast<double>(sums[c]) / static_cast<double>(pixels);
  }
  return out;
}

}  // namespace

void PolicyClient::open_log_file_(const std::string& log_path) {
  if (log_path.empty()) return;
  try {
    const auto resolved = expand_tilde(log_path);
    std::error_code ec;
    if (resolved.has_parent_path()) {
      std::filesystem::create_directories(resolved.parent_path(), ec);
      if (ec) {
        std::cerr << "[policy_client:" << name()
                  << "] failed to create log dir '"
                  << resolved.parent_path() << "': " << ec.message() << "\n";
        return;
      }
    }
    shared_->log_file.open(resolved, std::ios::out | std::ios::app);
    if (!shared_->log_file) {
      std::cerr << "[policy_client:" << name()
                << "] failed to open log file '" << resolved << "'\n";
      return;
    }
    shared_->log_t0 = std::chrono::steady_clock::now();
    std::cerr << "[policy_client:" << name()
              << "] JSONL log → " << resolved << "\n";
  } catch (const std::exception& e) {
    std::cerr << "[policy_client:" << name()
              << "] open_log_file_ threw: " << e.what() << "\n";
  }
}

void PolicyClient::log_request_(uint64_t seq,
                                std::chrono::steady_clock::time_point t_send,
                                const Observation& obs,
                                const ObservationDiagnostics& diag) {
  if (!shared_->log_file.is_open()) return;
  try {
    const auto ts = render_timestamps(t_send, shared_->log_t0);

    nlohmann::json line;
    line["event"] = "request";
    line["seq"] = seq;
    line["t_mono_s"] = ts.mono_s;
    line["t_wall"] = ts.wall_iso;

    // Diagnostics — emitted before the heavy payload fields so a casual `tail`
    // shows the small fields first. obs_collect_ms uses the same field name
    // as the openpi side so they collate cleanly even though the underlying
    // measurement differs (freshness wait vs. get_observation duration).
    line["obs_collect_ms"] = diag.freshness_wait_ms;
    line["cycle_interval_ms"] = diag.cycle_interval_ms;
    {
      nlohmann::json ages = nlohmann::json::object();
      for (const auto& [rec_id, age] : diag.ages_ms) {
        // JSON has no NaN; serialize missing records as null instead.
        if (std::isnan(age)) {
          ages[rec_id] = nullptr;
        } else {
          ages[rec_id] = age;
        }
      }
      line["obs_ages_ms"] = std::move(ages);
    }
    line["obs_skew_ms"] = diag.skew_ms;

    // State: flat concatenation in group order — the same field and shape the
    // wire-JSON version logged, so existing paired-log tooling keeps working.
    nlohmann::json state_arr = nlohmann::json::array();
    for (const auto& group : obs.state) {
      for (float v : group.values) state_arr.push_back(v);
    }
    line["state"] = std::move(state_arr);

    // Per-camera shape + per-channel mean. mean_per_ch describes the NEUTRAL
    // true-RGB bytes ([R, G, B] of the scene), not the wire bytes: against a
    // reference openpi client log (which records the wire's BGR training
    // quirk), channels 0 and 2 appear swapped BY DESIGN — not a bug.
    nlohmann::json images = nlohmann::json::object();
    for (const auto& img : obs.images) {
      nlohmann::json entry;
      entry["shape"] = {3, img.height, img.width};
      entry["mean_per_ch"] =
        per_channel_mean_hwc(img.rgb.data(), img.height, img.width);
      images[img.camera] = std::move(entry);
    }
    line["images"] = std::move(images);

    line["prompt"] = obs.task;
    {
      std::lock_guard<std::mutex> lk(shared_->log_mu);
      shared_->log_file << line.dump() << '\n';
      shared_->log_file.flush();
    }
  } catch (...) {
    // Logging is best-effort.
  }
}

void PolicyClient::log_response_(
    uint64_t seq,
    std::chrono::steady_clock::time_point t_recv,
    double rt_ms,
    const std::shared_ptr<const ActionChunk>& chunk) {
  if (!shared_->log_file.is_open() || !chunk) return;
  try {
    const auto ts = render_timestamps(t_recv, shared_->log_t0);
    nlohmann::json line;
    line["event"] = "response";
    line["seq"] = seq;
    line["t_mono_s"] = ts.mono_s;
    line["t_wall"] = ts.wall_iso;
    line["rt_ms"] = rt_ms;
    line["T"] = chunk->T;
    line["N"] = chunk->N;
    // No "dtype" field: the wire dtype is a transport-internal fact now (the
    // decoder normalizes f64 to f32); logging a constant here would fabricate
    // wire evidence in a log used for wire-parity diffs.

    auto row_to_json = [&](int t) {
      nlohmann::json arr = nlohmann::json::array();
      const std::size_t start =
        static_cast<std::size_t>(t) * static_cast<std::size_t>(chunk->N);
      for (int n = 0; n < chunk->N; ++n) {
        arr.push_back(chunk->data[start + static_cast<std::size_t>(n)]);
      }
      return arr;
    };
    if (chunk->T > 0) line["row0"] = row_to_json(0);
    if (chunk->T > 1) line["row1"] = row_to_json(1);
    if (chunk->T > 0) line["row_last"] = row_to_json(chunk->T - 1);
    line["chunk_seq"] = chunk->chunk_seq;

    // ─ Chunk shape diagnostics ─────────────────────────────────────────────
    // col_l1: per-column total L1 across the chunk. Identifies which joints
    // the policy is actually moving and by how much. Comparing this field
    // between openpi and SDK runs at matched seqs immediately surfaces
    // policy disagreement at the column level (e.g., one side is gripper-
    // active, the other isn't).
    if (chunk->T > 1 && chunk->N > 0) {
      std::vector<double> col_l1(static_cast<std::size_t>(chunk->N), 0.0);
      for (int t = 1; t < chunk->T; ++t) {
        const std::size_t row_a =
          static_cast<std::size_t>(t - 1) * static_cast<std::size_t>(chunk->N);
        const std::size_t row_b =
          static_cast<std::size_t>(t) * static_cast<std::size_t>(chunk->N);
        for (int n = 0; n < chunk->N; ++n) {
          col_l1[static_cast<std::size_t>(n)] +=
            std::abs(chunk->data[row_b + static_cast<std::size_t>(n)] -
                     chunk->data[row_a + static_cast<std::size_t>(n)]);
        }
      }
      line["col_l1"] = col_l1;
    }

    // row_l1_samples: per-row L1 across all columns at five quartile points
    // (rows 0,1 transition; T/4, T/2, 3T/4 transitions; T-1 transition).
    // Cheap shape-of-trajectory fingerprint, length 5.
    if (chunk->T > 1 && chunk->N > 0) {
      auto row_l1_at = [&](int t) {
        if (t <= 0 || t >= chunk->T) return 0.0;
        const std::size_t row_a =
          static_cast<std::size_t>(t - 1) * static_cast<std::size_t>(chunk->N);
        const std::size_t row_b =
          static_cast<std::size_t>(t) * static_cast<std::size_t>(chunk->N);
        double s = 0.0;
        for (int n = 0; n < chunk->N; ++n) {
          s += std::abs(chunk->data[row_b + static_cast<std::size_t>(n)] -
                        chunk->data[row_a + static_cast<std::size_t>(n)]);
        }
        return s;
      };
      const int T = chunk->T;
      nlohmann::json samples = nlohmann::json::array();
      samples.push_back(row_l1_at(1));
      samples.push_back(row_l1_at(std::max(1, T / 4)));
      samples.push_back(row_l1_at(std::max(1, T / 2)));
      samples.push_back(row_l1_at(std::max(1, (3 * T) / 4)));
      samples.push_back(row_l1_at(T - 1));
      line["row_l1_samples"] = std::move(samples);
    }

    // gripper_traj_per_arm: trajectory of each arm's last column across the
    // chunk, keyed by leader_id. Diagnoses the "block not released" case
    // directly: a chunk that doesn't drive the gripper toward an open value
    // means the policy itself isn't commanding a release. Convention: the
    // gripper is the last joint of each layout entry.
    if (chunk->T > 0 && chunk->N > 0) {
      nlohmann::json grip = nlohmann::json::object();
      for (const auto& row : cfg_.joint_layout) {
        if (row.joint_count <= 0) continue;
        const int col = row.joint_offset + row.joint_count - 1;
        if (col < 0 || col >= chunk->N) continue;
        nlohmann::json traj = nlohmann::json::array();
        for (int t = 0; t < chunk->T; ++t) {
          const std::size_t idx =
            static_cast<std::size_t>(t) * static_cast<std::size_t>(chunk->N) +
            static_cast<std::size_t>(col);
          traj.push_back(chunk->data[idx]);
        }
        grip[row.leader_id] = std::move(traj);
      }
      line["gripper_traj_per_arm"] = std::move(grip);
    }

    {
      std::lock_guard<std::mutex> lk(shared_->log_mu);
      shared_->log_file << line.dump() << '\n';
      shared_->log_file.flush();
    }
  } catch (...) {
    // Logging is best-effort.
  }
}

void PolicyClient::SharedState::log_tick(
    const std::string& face_id,
    std::chrono::steady_clock::time_point t,
    const std::vector<float>& action,
    const ChunkSlot::SampleInfo* info_ptr) noexcept {
  // Cheap guard before any allocation: tick logging is best-effort and
  // must add minimal overhead to the real-time face.read() path.
  if (!log_file.is_open()) return;
  try {
    const auto ts = render_timestamps(t, log_t0);
    nlohmann::json line;
    line["event"] = "tick";
    line["t_mono_s"] = ts.mono_s;
    line["t_wall"] = ts.wall_iso;
    line["face_id"] = face_id;
    line["action"] = action;
    if (info_ptr) {
      line["chunk_seq"] = info_ptr->chunk_seq;
      line["t_idx"] = info_ptr->t_idx;
      line["T"] = info_ptr->T;
      line["saturated"] = info_ptr->saturated;
      line["blend_active"] = info_ptr->blend_active;
    }
    {
      std::lock_guard<std::mutex> lk(log_mu);
      log_file << line.dump() << '\n';
      // No flush per tick — 60 lines/s × flush() would dominate CPU.
      // The file stream's internal buffer flushes on overflow; the
      // request/response paths flush(), which also flushes any buffered
      // tick lines, so on-disk completeness is bounded by inference
      // cadence (~2 s).
    }
  } catch (...) {
    // Logging is best-effort.
  }
}

void PolicyClient::warn_once_(const std::string& key, const std::string& message) {
  bool first = false;
  {
    std::lock_guard<std::mutex> lk(warn_mu_);
    auto [it, inserted] = warned_.emplace(key, true);
    (void)it;
    first = inserted;
  }
  if (!first) {
    return;
  }
  try {
    std::cerr << "[policy_client:" << name() << "] " << message << "\n";
  } catch (...) {
    // Logging failures are not propagated.
  }
}

void PolicyClient::warn_throttled_(const std::string& key,
                                   const std::string& message,
                                   std::chrono::milliseconds interval_ms) {
  const auto now = std::chrono::steady_clock::now();
  bool should_log = false;
  {
    std::lock_guard<std::mutex> lk(warn_mu_);
    auto it = warn_throttle_at_.find(key);
    if (it == warn_throttle_at_.end() || (now - it->second) >= interval_ms) {
      warn_throttle_at_[key] = now;
      should_log = true;
    }
  }
  if (!should_log) {
    return;
  }
  try {
    std::cerr << "[policy_client:" << name() << "] " << message << "\n";
  } catch (...) {
    // Logging failures are not propagated.
  }
}

// ── PolicyClient::Face ───────────────────────────────────────────────────────

PolicyClient::Face::Face(std::shared_ptr<SharedState> state, std::string id,
                         int joint_offset, int joint_count)
  : HardwareComponent(std::move(id)),
    state_(std::move(state)),
    joint_offset_(joint_offset),
    joint_count_(joint_count) {}

void PolicyClient::Face::apply_output_ema(std::vector<float>& out,
                                          std::vector<float>& prev,
                                          double alpha_arm,
                                          double alpha_gripper) noexcept {
  // EMA smoothing: layered on top of the arm's write_moving_time_s controller
  // filter to reject per-row chunk noise. Two alphas — one for the arm joints
  // and a separate one for the last joint (gripper by convention). The gripper
  // alpha defaults to 1.0 (pass-through) because gripper open/close is a fast
  // transient that must reach full extent within a few rows; filtering it like
  // the arm joints causes incomplete grasps. α=1.0 is a no-op on either side;
  // the filter activates only when at least one channel needs it.
  const bool filter_arm = (alpha_arm > 0.0 && alpha_arm < 1.0);
  const bool filter_grip = (alpha_gripper > 0.0 && alpha_gripper < 1.0);
  if (!(filter_arm || filter_grip)) {
    return;  // pure pass-through; do not even seed history
  }
  if (prev.size() == out.size()) {
    const float a_arm = static_cast<float>(alpha_arm);
    const float a_grip = static_cast<float>(alpha_gripper);
    const std::size_t grip_idx =
      out.empty() ? 0 : out.size() - 1;  // last joint of the slice
    for (std::size_t i = 0; i < out.size(); ++i) {
      const bool is_grip = (i == grip_idx);
      const float a = is_grip ? a_grip : a_arm;
      // Skip the blend entirely on channels whose alpha is 1.0 — that
      // preserves bit-for-bit pass-through and avoids needless float rounding
      // when only one of the two filters is active.
      if (a >= 1.0f) continue;
      const float prev_v = prev[i];
      // A non-finite history term would latch forever: NaN/Inf propagate
      // through out_t = a·row + (1-a)·prev to every future output. Pass the
      // current row value through instead of blending against a poisoned
      // previous output, so the filter self-heals on the next finite row.
      if (!std::isfinite(prev_v)) continue;
      out[i] = a * out[i] + (1.0f - a) * prev_v;
    }
  }
  // Write-back guard: cache only finite values, per channel. One non-finite
  // output must never become next tick's history term (which would latch);
  // a channel that is non-finite this tick keeps its last finite cached value.
  // On the first tick (or after reset) prev is empty, so seed it to the right
  // width first — this reproduces the original "seed from the first read"
  // behavior while dropping any non-finite element.
  if (prev.size() != out.size()) {
    prev.assign(out.size(), 0.0f);
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (std::isfinite(out[i])) {
      prev[i] = out[i];
    }
  }
}

std::vector<float> PolicyClient::Face::read() noexcept {
  try {
    const auto now = std::chrono::steady_clock::now();
    const auto info = state_->chunk_slot.sample_with_info(
      now,
      state_->total_n.load(std::memory_order_acquire),
      state_->control_rate_hz.load(std::memory_order_acquire));

    std::vector<float> out(static_cast<std::size_t>(joint_count_), 0.0f);
    const int avail = static_cast<int>(info.row.size());
    const int begin = std::min(joint_offset_, avail);
    const int end = std::min(joint_offset_ + joint_count_, avail);
    if (end > begin) {
      std::copy(info.row.begin() + begin, info.row.begin() + end, out.begin());
    }
    apply_output_ema(out, prev_out_,
                     state_->output_ema_alpha.load(std::memory_order_acquire),
                     state_->output_ema_alpha_gripper.load(
                       std::memory_order_acquire));
    // Per-tick action log. log_tick early-exits when no log file is open, so
    // this is essentially free in non-diagnostic runs. It reads the shared log
    // sink, never the client, so it is safe even if the client is gone.
    try {
      state_->log_tick(get_identifier(), now, out, &info);
    } catch (...) {
      // Logging must never propagate into the real-time control loop.
    }
    return out;
  } catch (...) {
    // Hold-last-action: NEVER command every joint to zero (a large, dangerous
    // motion). Return the last emitted output when we have one; otherwise an
    // empty vector, which the teleop loop treats as "no command this tick".
    try {
      if (!prev_out_.empty()) {
        return prev_out_;
      }
    } catch (...) {
      // fall through to the empty vector
    }
    return std::vector<float>{};
  }
}

}  // namespace trossen::hw::policy
