/**
 * @file rerun_observer.hpp
 * @brief Concrete Observer that streams records to a ReRun viewer.
 */

#ifndef TROSSEN_SDK__OBSERVER__RERUN_OBSERVER_HPP_
#define TROSSEN_SDK__OBSERVER__RERUN_OBSERVER_HPP_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "nlohmann/json.hpp"
#include "opencv2/core.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/observer/observer_base.hpp"

// Forward-declare so the rerun headers stay out of the public include surface.
namespace rerun {
class RecordingStream;
}  // namespace rerun

namespace trossen::observer {

namespace detail {

/**
 * @brief Resolved color-encoding tag for an ``ImageRecord`` payload.
 *
 * The producer-side ``encoding`` field is a string. ``dispatch_`` resolves it once per
 * subscription into this enum so the hot path uses an exhaustive ``switch`` and never
 * re-runs string comparisons. ``kUnresolved`` is the initial state before the first
 * ``ImageRecord`` arrives; ``kUnsupported`` is sticky for any unknown / mismatched
 * encoding string.
 */
enum class ColorEncoding {
  kUnresolved,
  kRgb8,
  kBgr8,
  kMono8,
  kUnsupported,
};

/**
 * @brief Resolve a producer-side encoding string to a ``ColorEncoding`` tag.
 *
 * Recognised encodings: ``"rgb8"``, ``"bgr8"``, ``"mono8"``. Anything else (including
 * the empty string) maps to ``kUnsupported``.
 */
ColorEncoding resolve_encoding(const std::string& encoding);

/**
 * @brief Convert an 8-bit color image to a contiguous RGB ``cv::Mat``.
 *
 * The returned ``cv::Mat`` is guaranteed to be ``CV_8UC3`` (RGB) and contiguous, or
 * empty on failure. The pixel data is owned by the returned ``cv::Mat`` and outlives
 * the call; the caller borrows ``.data`` directly into ``rerun::archetypes::Image``
 * without an intermediate scratch buffer.
 *
 * Supported encodings: ``kRgb8``, ``kBgr8``, ``kMono8``. ``kUnresolved`` and
 * ``kUnsupported`` return an empty ``cv::Mat``. A type-mismatched ``cv::Mat`` (e.g.
 * mono source tagged ``kRgb8``) also returns an empty ``cv::Mat``. An ``kRgb8`` input
 * that is non-contiguous (typically an ROI slice) returns an empty ``cv::Mat`` so the
 * caller can surface a distinct skip reason instead of borrowing into a row-stepped
 * view.
 */
cv::Mat mat_to_rgb(const cv::Mat& image, ColorEncoding encoding);

}  // namespace detail

/**
 * @brief Observer implementation that forwards records to a ReRun viewer.
 *
 * Owns one ``rerun::RecordingStream`` connected over gRPC. The worker dispatches each
 * record by concrete type:
 *
 *  - ``data::ImageRecord``       -> ``rerun::Image`` (and ``rerun::DepthImage`` if present)
 *  - ``data::JointStateRecord``  -> ``rerun::Scalars`` for positions / velocities / efforts
 *  - ``data::Odometry2DRecord``  -> ``rerun::Scalars`` for pose (x/y/theta) and body-frame
 *                                   twist (linear_x/linear_y/angular_z)
 *  - other record types are counted as skipped.
 *
 * A failed gRPC connect returns ``false`` from ``on_start()`` so the SessionManager can
 * mark the observer dead and continue recording.
 */
class RerunObserver : public ObserverBase {
public:
  /**
   * @brief Construct from a JSON configuration object.
   *
   * Expected fields:
   *  - ``type`` (string, required) - registry key ("rerun")
   *  - ``id`` (string, optional) - logging name; defaults to ``type``
   *  - ``rerun_url`` (string, optional) - gRPC URL of the ReRun viewer. Defaults to
   *    ``"rerun+http://127.0.0.1:9876/proxy"`` (matches rerun-cpp). Ignored when
   *    ``spawn`` is true.
   *  - ``app_id`` (string, optional) - ReRun application id; defaults to ``"trossen_sdk"``.
   *  - ``spawn`` (bool, optional) - when true, ``on_start()`` launches a local
   *    ReRun viewer process via ``RecordingStream::spawn()`` instead of connecting
   *    over gRPC. If a viewer is already listening on the default port, the stream
   *    is redirected to it (no duplicate processes). Defaults to false.
   *  - ``subscriptions`` (array, required) - each entry must have ``record_id`` (string)
   *    and ``throttle_hz`` (positive number).
   *
   * @param cfg JSON object (the raw JSON parsed by ``ObserverConfig``).
   * @throws std::runtime_error on missing/invalid required fields.
   */
  explicit RerunObserver(const nlohmann::json& cfg);
  ~RerunObserver() override;

  /// ReRun application id passed to the recording stream constructor.
  const std::string& app_id() const noexcept { return app_id_; }

  /// gRPC URL of the connected viewer.
  const std::string& rerun_url() const noexcept { return rerun_url_; }

  /// True if ``on_start()`` will launch a local viewer via ``RecordingStream::spawn``
  /// instead of connecting to a pre-launched one via ``connect_grpc``.
  bool spawn_enabled() const noexcept { return spawn_viewer_; }

  /// Records the worker reached but did not log (unsupported encoding, record type,
  /// or depth scale). Lets operators detect a silently-empty viewer.
  uint64_t skipped_frames() const noexcept {
    return skipped_frames_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Clear every subscribed entity tree at the start of a new episode.
   *
   * Logs a recursive ``rerun::archetypes::Clear`` to each ``record_id`` we have
   * dispatched data into, so the viewer drops the previous episode's history before
   * the new episode's first records arrive. Two problems this fixes:
   *
   *   - **Autoscale collapse on cross-episode pose jumps.** A new episode that starts
   *     at a very different pose from where the last one ended forces rerun's auto-Y
   *     to span both clusters, squashing the dense pre-jump data against the axis
   *     edge (it looks like the plot vanished). Clearing keeps each episode's plot
   *     on its own auto-Y range.
   *   - **Cross-episode line interpolation.** Without a clear, scalar plots draw a
   *     long segment connecting the last sample of episode N to the first sample of
   *     episode N+1; that single steep line is visually misleading.
   *
   * Per-record on-disk capture (MCAP, etc.) is unaffected - this only clears what the
   * live viewer renders. Dispatched outside ``episode_mutex_`` per the
   * ``ObserverBase::on_episode_started`` contract.
   */
  void on_episode_started(uint32_t episode_index) noexcept override;

protected:
  /// Open the ReRun gRPC connection. Returns ``false`` on transport failure.
  bool on_start() override;

  /// Drop the ReRun recording stream.
  void on_stop() override;

private:
  /// Worker-thread dispatch entry point. Captured into each subscription's handler.
  void dispatch_(const std::string& record_id,
                 const std::shared_ptr<data::RecordBase>& rec);

  /**
   * @brief Per-subscription cached state owned by ``dispatch_``.
   *
   * The dispatcher resolves derived quantities lazily on the first matching record so
   * the hot path does no string parsing on steady-state frames. Entries are inserted
   * lazily by ``dispatch_`` (encoding is unknown at construction; we don't pre-populate
   * here for that reason).
   *
   * Synchronisation: ``dispatch_`` (worker thread) inserts into ``subscription_state_``
   * on first sight of each ``record_id``, and ``on_episode_started`` (caller thread)
   * iterates the map to log a Clear per entity. Concurrent insert-vs-iterate would be
   * a data race, so both code paths take ``subscription_state_mutex_`` only long
   * enough to lookup-or-insert (``dispatch_``) or snapshot the keys (``on_episode_started``).
   * ``std::unordered_map`` guarantees that references to existing elements stay valid
   * across rehashes, so ``dispatch_`` releases the lock before touching the entry's
   * fields - no contention on the hot path beyond the brief map lookup.
   */
  struct PerSubscriptionState {
    /// Resolved color encoding for ``ImageRecord`` payloads on this subscription.
    /// Starts at ``kUnresolved``; on the first ``ImageRecord``, ``dispatch_`` calls
    /// ``resolve_encoding`` and pins the result. Sticks at ``kUnsupported`` once set.
    detail::ColorEncoding encoding{detail::ColorEncoding::kUnresolved};

    /// Cached entity paths derived from ``record_id``. Each group is lazily populated
    /// on the first matching record (image / joint-state / odometry) so the steady-state
    /// hot path performs no string concatenation per frame. The image-color path is
    /// just ``record_id`` itself (used bare), so no caching is needed for it.
    struct ImagePaths { std::string depth; };
    struct JointPaths { std::string positions, velocities, efforts; };
    struct OdomPaths {
      std::string pose_x, pose_y, pose_theta,
                  twist_lx, twist_ly, twist_wz;
    };

    std::optional<ImagePaths> image_paths;
    std::optional<JointPaths> joint_paths;
    std::optional<OdomPaths> odom_paths;

    /// Optional per-subscription field filter. When set (non-empty), only the
    /// listed field names are forwarded to ReRun for ``JointStateRecord`` (subset
    /// of ``positions`` / ``velocities`` / ``efforts``). Empty / nullopt means
    /// "log all available fields" (default, backward compatible).
    std::optional<std::unordered_set<std::string>> joint_field_filter;
  };

  std::string rerun_url_;
  std::string app_id_;
  bool spawn_viewer_{false};

  // Writes (on_start/on_stop) and reads (dispatch_ on the worker) never overlap in time
  // because ObserverBase::start joins-before-on_start and joins-before-on_stop; no
  // synchronization required.
  std::unique_ptr<rerun::RecordingStream> rec_;

  // Per-instance set of skip reasons already logged. The worker thread is the only writer
  // (dispatch_ runs on this observer's single worker), so no synchronisation is required.
  // Kept per-instance so two RerunObserver instances do not share a static and race.
  std::unordered_set<std::string> logged_skip_reasons_;

  // Lazy per-record_id cache. Keyed by the ``record_id`` argument passed to
  // ``dispatch_``; ``operator[]`` inserts a default-constructed entry on first sight.
  // ``dispatch_`` (worker thread) inserts; ``on_episode_started`` (caller thread)
  // iterates. ``subscription_state_mutex_`` serialises insert-vs-iterate; see the
  // ``PerSubscriptionState`` docstring for the locking discipline.
  std::unordered_map<std::string, PerSubscriptionState> subscription_state_;
  std::mutex subscription_state_mutex_;

  std::atomic<uint64_t> skipped_frames_{0};
};

}  // namespace trossen::observer

#endif  // TROSSEN_SDK__OBSERVER__RERUN_OBSERVER_HPP_
