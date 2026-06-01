/**
 * @file rerun_observer.hpp
 * @brief Concrete Observer that streams records to a ReRun viewer.
 */

#ifndef TROSSEN_SDK__OBSERVER__RERUN_OBSERVER_HPP_
#define TROSSEN_SDK__OBSERVER__RERUN_OBSERVER_HPP_

#include <atomic>
#include <cstdint>
#include <memory>
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
   *    ``"rerun+http://127.0.0.1:9876/proxy"`` (matches rerun-cpp).
   *  - ``app_id`` (string, optional) - ReRun application id; defaults to ``"trossen_sdk"``.
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

  /// Records the worker reached but did not log (unsupported encoding, record type,
  /// or depth scale). Lets operators detect a silently-empty viewer.
  uint64_t skipped_frames() const noexcept {
    return skipped_frames_.load(std::memory_order_relaxed);
  }

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
   * Worker-thread invariant: ``RerunObserver`` has a single worker, and ``dispatch_``
   * is the sole reader/writer of ``subscription_state_``. No synchronisation required.
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
  };

  std::string rerun_url_;
  std::string app_id_;

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
  // Only ``dispatch_`` (worker thread) touches this map, so no synchronisation is
  // required.
  std::unordered_map<std::string, PerSubscriptionState> subscription_state_;

  std::atomic<uint64_t> skipped_frames_{0};
};

}  // namespace trossen::observer

#endif  // TROSSEN_SDK__OBSERVER__RERUN_OBSERVER_HPP_
