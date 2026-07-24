/**
 * @file action_chunk.hpp
 * @brief Action chunk POD and slot publishing actions to TeleopController readers.
 */

#ifndef TROSSEN_SDK__HW__POLICY__ACTION_CHUNK_HPP_
#define TROSSEN_SDK__HW__POLICY__ACTION_CHUNK_HPP_

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace trossen::hw::policy {

/**
 * @brief Fixed-size [T x N] row-major chunk of commanded actions.
 *
 * Populated by the PolicyClient inference thread after a successful
 * server round-trip and then published into a ChunkSlot. Immutable once
 * published.
 */
struct ActionChunk {
  int T{0};
  int N{0};
  std::vector<float> data;
  uint64_t chunk_seq{0};
  std::chrono::steady_clock::time_point received_at{};
  /// Absolute Timestep Clock tick at which row 0 should play. Lets ChunkSlot
  /// align a chunk to the clock on take-over (skip rows already in the past,
  /// discard an all-past chunk). LeRobot stamps the server-scheduled value;
  /// openpi stamps the timestep of the observation that produced the chunk.
  int64_t base_timestep{0};

  /**
   * @brief Return the action row at index t.
   * @param t row index in [0, T).
   * @throws std::out_of_range if t is outside [0, T), or if @c data is shorter
   *         than the [t*N, t*N+N) slice it would return.
   */
  [[nodiscard]] std::vector<float> row(int t) const {
    if (t < 0 || t >= T) {
      throw std::out_of_range(
        "ActionChunk::row: t=" + std::to_string(t) +
        " out of range [0, " + std::to_string(T) + ")");
    }
    const std::size_t start = static_cast<std::size_t>(t) * static_cast<std::size_t>(N);
    // data may be shorter than T*N on a malformed server chunk; slicing past
    // data.end() would be UB, so reject it rather than build from bad iterators.
    if (start + static_cast<std::size_t>(N) > data.size()) {
      throw std::out_of_range(
        "ActionChunk::row: data.size()=" + std::to_string(data.size()) +
        " too short for row " + std::to_string(t) + " of width " +
        std::to_string(N));
    }
    return std::vector<float>(data.begin() + start, data.begin() + start + N);
  }
};

/**
 * @brief Thread-safe holder for the playing ActionChunk, a pending successor,
 *        and the last commanded row.
 *
 * Consume-fully contract: a chunk handed to swap_in() while another chunk is
 * still playing is parked in @p pending_ and only promoted to @p latest_ once
 * @p latest_ has been sampled past its last row. The pending slot is depth-1:
 * newer pendings replace older ones (the policy server's last word wins).
 *
 * Pass a null pointer to swap_in() to reset the slot entirely (clears both
 * latest_ and pending_); subsequent samples fall back to the cached last
 * commanded row.
 *
 * Threading (single-consumer contract): exactly one thread may call the
 * sample family (sample / sample_with_info). It may run concurrently with
 * swap_in / swap_in_aligned / peek / exhaustion_time called from the
 * inference thread — those are all mu_-guarded. Concurrent calls into the
 * sample family from more than one thread are race-free (mu_ protects the
 * state) but logically undefined: the samplers interleave playback
 * advancement (playback_start_, last_cmd_, t_idx progression) and corrupt
 * each other's notion of "where playback is." Use one sampler.
 */
class ChunkSlot {
 public:
  ChunkSlot() = default;
  ChunkSlot(const ChunkSlot&) = delete;
  ChunkSlot& operator=(const ChunkSlot&) = delete;
  ChunkSlot(ChunkSlot&&) = delete;
  ChunkSlot& operator=(ChunkSlot&&) = delete;

  /// Optional linear cross-fade window applied at chunk-boundary promotions
  /// (pending → latest). For @p seconds after a promotion, sample() blends the
  /// last commanded row from the outgoing chunk with the new chunk's row using
  /// alpha = clamp(elapsed/seconds, 0, 1). Default 0 = no blend (hard step).
  void set_boundary_blend_s(double seconds) noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    boundary_blend_s_ = seconds > 0.0 ? seconds : 0.0;
  }

  /// Column indices (into the chunk's per-row vector) that bypass the
  /// boundary cross-fade. Intended for gripper channels: the cross-fade is
  /// designed to smooth arm-joint transitions across chunk promotions, but
  /// gripper open/close is a fast transient whose value the policy may
  /// only command for a few rows of the new chunk. Blending dampens that
  /// command and produces weak grasps. Skipped channels jump straight to
  /// the new chunk's row value at the boundary; arm channels continue to
  /// blend as before.
  void set_boundary_blend_skip_indices(std::vector<int> indices) noexcept {
    try {
      std::lock_guard<std::mutex> lk(mu_);
      boundary_blend_skip_indices_ = std::move(indices);
    } catch (...) {
      // noexcept contract; nothing actionable on lock failure.
    }
  }

  /**
   * @brief Publish a new chunk under the consume-fully contract.
   *
   * - Null clears both latest_ and pending_ (hold-last-action via last_cmd_).
   * - With no current chunk, the new chunk becomes latest_ and starts playing.
   * - With a current chunk still playing, the new chunk lands in pending_
   *   (replacing any older pending) and is promoted by sample() once the
   *   current chunk is exhausted.
   */
  void swap_in(std::shared_ptr<const ActionChunk> chunk) noexcept {
    try {
      std::lock_guard<std::mutex> lk(mu_);
      if (!chunk) {
        latest_.reset();
        pending_.reset();
        blend_from_.clear();
        return;
      }
      if (!latest_) {
        // Anchor row 0 at the chunk's receipt instant (not the first sample),
        // so playback stays aligned to wall time: any rows whose play instant
        // already passed during inference latency are skipped by sample()'s
        // t_idx, rather than replayed from row 0. received_at and the sample
        // `now` are both steady_clock, so the offset is meaningful.
        playback_start_ = chunk->received_at;
        latest_ = std::move(chunk);
        return;
      }
      pending_ = std::move(chunk);
    } catch (...) {
      // lock_guard ctor is not expected to throw; swallow per noexcept.
    }
  }

  /**
   * @brief Aligned take-over: install @p chunk as the playing chunk at once.
   *
   * Unlike swap_in's consume-fully parking, this replaces @c latest_
   * immediately (the async-overlap / drain-threshold theta>0 path). Row 0 is
   * anchored at @p playback_start on the wall clock — typically
   * ``epoch + base_timestep / rate`` — so the next sample() serves the row
   * matching "now" and skips rows already in the past. The caller is
   * responsible for the all-past discard decision (it owns the timestep clock);
   * a @p playback_start far enough in the past simply makes sample() clamp at
   * the last row until a successor arrives.
   *
   * Stages a boundary cross-fade from the last commanded row (if configured and
   * width-compatible), starting at @p takeover_now, so the discontinuity
   * between the outgoing row and the new chunk's aligned row is smoothed.
   *
   * @param chunk          New chunk to play immediately (no-op if null).
   * @param playback_start Wall-clock anchor for the chunk's row 0.
   * @param takeover_now   Instant of take-over; the cross-fade origin.
   */
  void swap_in_aligned(
      std::shared_ptr<const ActionChunk> chunk,
      std::chrono::steady_clock::time_point playback_start,
      std::chrono::steady_clock::time_point takeover_now) noexcept {
    try {
      if (!chunk) return;
      std::lock_guard<std::mutex> lk(mu_);
      stage_blend_locked_(chunk->N, takeover_now);
      latest_ = std::move(chunk);
      pending_.reset();
      playback_start_ = playback_start;
    } catch (...) {
      // noexcept contract; nothing actionable on lock failure.
    }
  }

  /**
   * @brief Sample the action row corresponding to time @p now.
   *
   * Rows are returned in order 0..T-1 driven by @p control_rate_hz. When the
   * current chunk is exhausted and a pending chunk is queued, the pending is
   * promoted to latest_ (playback restarts at row 0). When no pending exists,
   * the last row is held indefinitely. Updates the cached last commanded row
   * so swap_in(nullptr) keeps returning a sensible value.
   *
   * Falls back to a zero vector (sized to total_n, or 1 if total_n is
   * non-positive) on configuration mismatch, missing chunk, or internal
   * failure. Never propagates exceptions.
   *
   * Single-consumer: only one thread may call this (see class doc). The
   * const-with-mutable playback advancement is intentional — playback state
   * is logically internal and every access is guarded by mu_.
   *
   * @param now             Sample timestamp (typically steady_clock::now()).
   * @param total_n         Expected width of the action row.
   * @param control_rate_hz Row-selection rate; must be > 0.
   * @return action row of length max(total_n, 1).
   */
  [[nodiscard]] std::vector<float> sample(
    std::chrono::steady_clock::time_point now,
    int total_n,
    double control_rate_hz) const noexcept {
    // Returns a freshly allocated row; a fill-into-buffer overload could avoid the alloc.
    try {
      std::lock_guard<std::mutex> lk(mu_);

      // control_rate_hz is a caller/config error, not a chunk defect: hold,
      // never discard a chunk over it.
      if (control_rate_hz <= 0.0) {
        return hold_last_locked_(total_n);
      }
      // An unplayable latest_ (degenerate dims, wrong width, or data shorter
      // than T*N on a malformed server chunk) must not wedge the slot in
      // hold-last: discard it and pull its successor so the slot self-heals,
      // otherwise a valid pending_ would never be promoted. This tick still
      // holds the last row; the next tick re-checks the freshly promoted chunk.
      if (latest_ && !chunk_playable_(*latest_, total_n)) {
        latest_ = std::move(pending_);
        pending_.reset();
        if (latest_) playback_start_ = now;
        return hold_last_locked_(total_n);
      }
      if (!latest_) {
        return hold_last_locked_(total_n);
      }

      auto t_idx_for = [&](std::chrono::steady_clock::time_point start) {
        const double dt_s =
          std::chrono::duration<double>(now - start).count();
        return static_cast<int64_t>(std::floor(dt_s * control_rate_hz));
      };

      int64_t t_idx = t_idx_for(playback_start_);

      // Promote pending → latest as soon as the current chunk is exhausted.
      if (t_idx >= latest_->T && pending_) {
        stage_blend_locked_(pending_->N, now);
        latest_ = std::move(pending_);
        pending_.reset();
        playback_start_ = now;
        t_idx = 0;
        // The promoted successor must be playable at the configured width; the
        // top-of-function guard only checked the outgoing chunk. An unplayable
        // successor would emit a wrong-length row (or slice past data.end()),
        // so hold the last commanded row instead (the next sample re-checks the
        // guard and self-heals).
        if (!chunk_playable_(*latest_, total_n)) {
          return hold_last_locked_(total_n);
        }
      }
      if (t_idx < 0) {
        t_idx = 0;
      }
      if (t_idx >= latest_->T) {
        t_idx = latest_->T - 1;
      }

      std::vector<float> row = latest_->row(static_cast<int>(t_idx));

      // Linear cross-fade with the outgoing chunk's last commanded row.
      if (!blend_from_.empty() && blend_from_.size() == row.size()) {
        const double dt =
          std::chrono::duration<double>(now - blend_start_).count();
        if (dt < boundary_blend_s_) {
          const double alpha = dt / boundary_blend_s_;
          const double one_minus = 1.0 - alpha;
          for (std::size_t i = 0; i < row.size(); ++i) {
            if (is_blend_skip_locked_(static_cast<int>(i))) continue;
            row[i] = static_cast<float>(
              one_minus * static_cast<double>(blend_from_[i]) +
              alpha * static_cast<double>(row[i]));
          }
        } else {
          // Blend window closed; release the carried row to skip the math on
          // every subsequent tick within this chunk.
          blend_from_.clear();
        }
      }

      last_cmd_ = row;
      return row;
    } catch (...) {
      const std::size_t n =
        total_n > 0 ? static_cast<std::size_t>(total_n) : std::size_t{1};
      return std::vector<float>(n, 0.0f);
    }
  }

  /**
   * @brief Sample result enriched with playback diagnostics.
   *
   * Returned by ``sample_with_info``. Carries the row, plus the per-tick
   * metadata callers need to correlate this row with the chunk boundary
   * (which chunk_seq it came from, which t_idx within the chunk, whether
   * the slot is currently saturating at row T-1, whether a boundary
   * cross-fade is active).
   */
  struct SampleInfo {
    /// The action row returned to the caller (length total_n).
    std::vector<float> row;
    /// Sequence number of the chunk this row was drawn from. Zero if no
    /// chunk is playing (the caller is receiving a hold-last-action vector).
    uint64_t chunk_seq{0};
    /// Row index within the chunk, clamped to [0, T-1]. -1 when no chunk.
    int t_idx{-1};
    /// Chunk depth. Zero when no chunk.
    int T{0};
    /// True when @c t_idx was clamped because the chunk has exhausted and
    /// no pending successor is available (slot is saturating at row T-1).
    bool saturated{false};
    /// True when the boundary cross-fade is still mixing the outgoing
    /// chunk's last row into the new chunk's row at this sample instant.
    bool blend_active{false};
  };

  /**
   * @brief Same playback contract as ``sample`` but returns diagnostics.
   *
   * Used by ``PolicyClient::Face::read`` for per-tick action logging so a
   * post-run analysis can identify chunk boundaries, exhaust-and-hold
   * windows, and cross-fade transitions in the actual command stream sent
   * to the followers.
   *
   * Single-consumer: only one thread may call this (see class doc). The
   * const-with-mutable playback advancement is intentional — playback state
   * is logically internal and every access is guarded by mu_.
   */
  [[nodiscard]] SampleInfo sample_with_info(
    std::chrono::steady_clock::time_point now,
    int total_n,
    double control_rate_hz) const noexcept {
    // Returns a freshly allocated row; a fill-into-buffer overload could avoid the alloc.
    SampleInfo info;
    try {
      std::lock_guard<std::mutex> lk(mu_);

      // control_rate_hz is a caller/config error, not a chunk defect: hold,
      // never discard a chunk over it.
      if (control_rate_hz <= 0.0) {
        info.row = hold_last_locked_(total_n);
        return info;
      }
      // An unplayable latest_ (degenerate dims, wrong width, or data shorter
      // than T*N on a malformed server chunk) must not wedge the slot: discard
      // it and pull its successor so the slot self-heals (mirror of sample()).
      if (latest_ && !chunk_playable_(*latest_, total_n)) {
        latest_ = std::move(pending_);
        pending_.reset();
        if (latest_) playback_start_ = now;
        info.row = hold_last_locked_(total_n);
        return info;
      }
      if (!latest_) {
        info.row = hold_last_locked_(total_n);
        return info;
      }

      auto t_idx_for = [&](std::chrono::steady_clock::time_point start) {
        const double dt_s =
          std::chrono::duration<double>(now - start).count();
        return static_cast<int64_t>(std::floor(dt_s * control_rate_hz));
      };

      int64_t t_idx = t_idx_for(playback_start_);

      if (t_idx >= latest_->T && pending_) {
        stage_blend_locked_(pending_->N, now);
        latest_ = std::move(pending_);
        pending_.reset();
        playback_start_ = now;
        t_idx = 0;
        // See sample(): an unplayable promoted successor must not reach the
        // followers — hold the last commanded row instead.
        if (!chunk_playable_(*latest_, total_n)) {
          info.row = hold_last_locked_(total_n);
          return info;
        }
      }
      if (t_idx < 0) t_idx = 0;
      const bool saturated_local = (t_idx >= latest_->T);
      if (saturated_local) t_idx = latest_->T - 1;

      std::vector<float> row = latest_->row(static_cast<int>(t_idx));

      bool blend_active_local = false;
      if (!blend_from_.empty() && blend_from_.size() == row.size()) {
        const double dt =
          std::chrono::duration<double>(now - blend_start_).count();
        if (dt < boundary_blend_s_) {
          const double alpha = dt / boundary_blend_s_;
          const double one_minus = 1.0 - alpha;
          for (std::size_t i = 0; i < row.size(); ++i) {
            if (is_blend_skip_locked_(static_cast<int>(i))) continue;
            row[i] = static_cast<float>(
              one_minus * static_cast<double>(blend_from_[i]) +
              alpha * static_cast<double>(row[i]));
          }
          blend_active_local = true;
        } else {
          blend_from_.clear();
        }
      }

      last_cmd_ = row;
      info.row = std::move(row);
      info.chunk_seq = latest_->chunk_seq;
      info.t_idx = static_cast<int>(t_idx);
      info.T = latest_->T;
      info.saturated = saturated_local;
      info.blend_active = blend_active_local;
      return info;
    } catch (...) {
      const std::size_t n =
        total_n > 0 ? static_cast<std::size_t>(total_n) : std::size_t{1};
      info.row.assign(n, 0.0f);
      return info;
    }
  }

  /**
   * @brief Snapshot the currently-playing chunk pointer (test/diagnostic).
   *
   * Returns latest_ — the chunk whose rows sample() is currently serving. A
   * pending successor (if any) is not visible through this accessor.
   */
  [[nodiscard]] std::shared_ptr<const ActionChunk> peek() const noexcept {
    try {
      std::lock_guard<std::mutex> lk(mu_);
      return latest_;
    } catch (...) {
      return nullptr;
    }
  }

  /**
   * @brief Expected wall-clock time at which the currently-playing chunk
   *        will exhaust (its last row's tick boundary passes).
   *
   * Used by the policy inference loop to time observation packing against
   * chunk consumption rather than wall-clock — matching openpi's
   * synchronous "request new chunk when the previous one is consumed"
   * semantics. Without this, the SDK packs observations mid-chunk and the
   * policy returns chunks whose row 0 is computed for an earlier arm
   * position than what's actually executing when row 0 plays, producing
   * visible backward corrections at every chunk boundary.
   *
   * @param control_rate_hz Row-sampling rate the inference loop will use.
   * @return Time point of expected exhaustion, or a default-constructed
   *         time point (epoch zero) when no chunk is playing or
   *         @p control_rate_hz is invalid.
   */
  [[nodiscard]] std::chrono::steady_clock::time_point exhaustion_time(
      double control_rate_hz) const noexcept {
    try {
      std::lock_guard<std::mutex> lk(mu_);
      if (!latest_ || latest_->T <= 0 || control_rate_hz <= 0.0) {
        return {};
      }
      const double duration_s =
        static_cast<double>(latest_->T) / control_rate_hz;
      return playback_start_ + std::chrono::nanoseconds(
        static_cast<int64_t>(duration_s * 1e9));
    } catch (...) {
      return {};
    }
  }

 private:
  // A chunk is playable only if its dims are positive, its width matches the
  // configured action width, and its data buffer actually holds T*N floats.
  // The last check guards against a malformed server chunk whose data is
  // shorter than advertised — row() would otherwise slice past data.end().
  static bool chunk_playable_(const ActionChunk& c, int total_n) noexcept {
    return c.T > 0 && c.N > 0 && c.N == total_n &&
           c.data.size() ==
             static_cast<std::size_t>(c.T) * static_cast<std::size_t>(c.N);
  }

  // Requires mu_ held. Linear scan is fine — the skip list is at most a
  // handful of indices (one gripper per arm), so a sorted-vector + binary
  // search would only add code complexity without measurable benefit.
  bool is_blend_skip_locked_(int idx) const {
    for (int v : boundary_blend_skip_indices_) {
      if (v == idx) return true;
    }
    return false;
  }

  // Requires mu_ held. Stage (or clear) a boundary cross-fade from the last
  // commanded row. The blend is staged only when a window is configured and
  // the last commanded row matches the incoming chunk's width; otherwise any
  // pending blend is cleared. @p origin is the instant the blend starts —
  // take-over time for swap_in_aligned, sample time for an in-sample promotion.
  void stage_blend_locked_(
      int new_n, std::chrono::steady_clock::time_point origin) const {
    if (boundary_blend_s_ > 0.0 &&
        last_cmd_.size() == static_cast<std::size_t>(new_n)) {
      blend_from_ = last_cmd_;
      blend_start_ = origin;
    } else {
      blend_from_.clear();
    }
  }

  // Requires mu_ held. Returns last_cmd_ resized to total_n if needed.
  std::vector<float> hold_last_locked_(int total_n) const {
    const std::size_t n =
      total_n > 0 ? static_cast<std::size_t>(total_n) : std::size_t{1};
    if (last_cmd_.size() != n) {
      last_cmd_.assign(n, 0.0f);
    }
    return last_cmd_;
  }

  mutable std::mutex mu_;
  mutable std::shared_ptr<const ActionChunk> latest_;
  mutable std::shared_ptr<const ActionChunk> pending_;
  mutable std::chrono::steady_clock::time_point playback_start_{};
  mutable std::vector<float> last_cmd_;

  // Cross-fade state. blend_from_ is populated at promotion and cleared once
  // the window elapses (or by swap_in(nullptr)). All accessed under mu_.
  double boundary_blend_s_{0.0};
  mutable std::vector<float> blend_from_;
  mutable std::chrono::steady_clock::time_point blend_start_{};
  /// Column indices that skip the boundary blend (gripper channels). Empty
  /// by default, meaning the blend covers every channel.
  std::vector<int> boundary_blend_skip_indices_;
};

}  // namespace trossen::hw::policy

#endif  // TROSSEN_SDK__HW__POLICY__ACTION_CHUNK_HPP_
