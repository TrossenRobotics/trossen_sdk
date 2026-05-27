/**
 * @file adamo_observer.hpp
 * @brief Observer that publishes local records onto Adamo pubsub.
 *
 * EXPERIMENTAL. Streams a local robot onto Adamo so a remote Adamo client
 * (e.g. the VR viewer at operate.adamohq.com) can watch it. One observer can
 * serve every arm + camera of a multi-arm robot under a single robot prefix.
 *
 * Subscribes to one or more local records and publishes each onto Adamo:
 *  - ``JointStateRecord`` is encoded via the ``trossen_adamo::wire`` codecs
 *    and pushed through a ``LatestPublisher`` to ``<robot>/<arm>/state`` (or
 *    ``/effort``), keyed by arm name (the subscription ``record_id``).
 *  - ``ImageRecord`` is converted to BGRA and pushed onto an
 *    ``adamo::VideoTrack`` opened against a per-observer ``adamo::Robot``.
 *
 * Scope of this experimental cut:
 *   - Joint-state codec is 7-DOF only (``trossen_adamo::wire::kNumJoints``).
 */

#ifndef TROSSEN_SDK__OBSERVER__ADAMO_OBSERVER_HPP_
#define TROSSEN_SDK__OBSERVER__ADAMO_OBSERVER_HPP_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/observer/observer_base.hpp"

// Forward declare to keep Adamo SDK headers out of the public include surface.
// Defined in <adamo/adamo.hpp>, pulled in only by the .cpp.
namespace adamo {
class Session;
class Robot;
class VideoTrack;
}  // namespace adamo
namespace trossen_adamo {
class LatestPublisher;
}  // namespace trossen_adamo

namespace trossen::observer {

/**
 * @brief Per-subscription publish target (what topic to write a record onto).
 *
 * Joint topics use a uniform ``<robot>/<arm>/<leaf>`` scheme so a single
 * observer can stream every arm of a multi-arm robot under one robot prefix,
 * keyed by arm name (the subscription's ``record_id``):
 *  - ``kJointState``  -> ``<robot>/<arm>/state``,  ``wire::encode_state``
 *                        (positions + velocities).
 *  - ``kJointEffort`` -> ``<robot>/<arm>/effort``, ``wire::encode_efforts``.
 *  - ``kCamera``      -> ``<robot>`` namespaced video track ``track_name``,
 *                        opened on an ``adamo::Robot`` and fed BGRA frames.
 *
 * Resolved from the JSON ``topic`` string on each subscription entry.
 */
enum class AdamoPublishTopic {
  kJointState,
  kJointEffort,
  kCamera,
};

/**
 * @brief Observer that publishes local records onto Adamo pubsub.
 *
 * Lifecycle parallels @ref RerunObserver: ``on_start()`` opens the
 * ``adamo::Session`` (joint-state path) and/or ``adamo::Robot`` plus per-track
 * ``adamo::VideoTrack`` (camera path), ``on_stop()`` drains them. Each
 * subscription's handler runs on the shared observer worker, dispatches on the
 * resolved ``PublishTarget`` topic, and ships the payload to the matching
 * publisher / video track.
 *
 * Joint-state publish latency is decoupled from the observer worker via
 * ``LatestPublisher``'s internal thread, so a slow network never stalls the
 * worker. Camera publish goes direct to ``VideoTrack::send`` — the Adamo
 * pipeline does its own encoding off-thread, but a stalled send still blocks
 * the worker. Treat camera as best-effort.
 */
class AdamoObserver : public ObserverBase {
public:
  /**
   * @brief Construct from a JSON configuration object.
   *
   * Required fields:
   *  - ``type`` (string) - registry key (``"adamo"``).
   *  - ``robot`` (string) - robot identifier that becomes the topic prefix.
   *    Must match the peer's ``robot``.
   *  - ``subscriptions`` (array) - per-record subscription entries, each with
   *    ``record_id``, ``throttle_hz``, and ``topic`` (one of ``"state"``,
   *    ``"effort"``, ``"camera"``). Joint topics resolve to
   *    ``<robot>/<record_id>/<state|effort>``, so one observer streams every
   *    arm of a multi-arm robot under one robot prefix, keyed by arm name -
   *    no second observer / second robot prefix needed. Camera subscriptions
   *    additionally require ``track_name`` (string) and
   *    ``width`` / ``height`` / ``fps`` / ``bitrate_kbps`` (positive integers).
   *
   * Optional fields:
   *  - ``id`` (string) - logging name; defaults to ``type``.
   *  - ``protocol`` (string) - ``"quic"`` (default), ``"udp"``, or ``"tcp"``.
   *  - ``api_key_env`` (string) - env var holding the Adamo API key;
   *    defaults to ``"ADAMO_API_KEY"``.
   *
   * @throws std::runtime_error on missing/invalid fields.
   */
  explicit AdamoObserver(const nlohmann::json& cfg);
  ~AdamoObserver() override;

  /// Configured robot/topic prefix (e.g. ``"trossen_stationary_ai"``).
  const std::string& robot() const noexcept { return robot_; }

  /// Records the worker reached but did not publish (wrong record type, joint
  /// count mismatch, encoder threw). Useful for spotting a silently-empty bus.
  uint64_t skipped_frames() const noexcept {
    return skipped_frames_.load(std::memory_order_relaxed);
  }

  /// True once any AdamoObserver instance has started an ``adamo::Robot``
  /// video pipeline in this process. The pipeline thread runs forever (the
  /// SDK has no stop hook) and is detached at teardown, which makes the C++
  /// global-destructor phase deadlock. Callers that own ``main()`` can query
  /// this after their normal shutdown and ``std::_exit`` to bypass the hang.
  /// Process-wide and latched: never cleared, since the pipeline never stops.
  static bool video_pipeline_active() noexcept;

  /// Per-subscription state resolved at construction time. Public so the
  /// anonymous-namespace ``make_target`` helper in the .cpp can return it.
  struct PublishTarget {
    AdamoPublishTopic topic{AdamoPublishTopic::kJointState};
    /// For kJointState / kJointEffort: fully-qualified Adamo topic name
    /// (e.g. ``"trossen_stationary_ai/leader_left/state"``). For kCamera:
    /// unused (video tracks are keyed by track_name on the Robot).
    std::string topic_name;

    // ── Camera-only fields (kCamera) ────────────────────────────────────────
    /// VideoTrack name passed to ``Robot::video``. Operator UI groups tracks
    /// by this name ("main"/"front"/"rear"/etc.).
    std::string track_name;
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t fps{0};
    std::uint32_t bitrate_kbps{0};
  };

protected:
  bool on_start() override;
  void on_stop() override;

private:
  /// Worker-thread dispatch entry point captured into each subscription's
  /// handler. Resolves the topic, encodes the record, and hands the bytes off
  /// to the matching ``LatestPublisher`` or ``VideoTrack``.
  void dispatch_(const std::string& record_id,
                 const std::shared_ptr<data::RecordBase>& rec);

  /// True if any subscription's resolved topic is ``kCamera``. Drives whether
  /// on_start() opens an ``adamo::Robot`` + run-thread.
  bool has_camera_subscription_() const noexcept;
  /// True if any subscription is a joint-state pubsub topic. Drives whether
  /// on_start() opens an ``adamo::Session``.
  bool has_session_subscription_() const noexcept;

  // ── Configuration (set in ctor, read-only after) ──────────────────────────
  std::string robot_;
  std::string protocol_;
  std::string api_key_env_;

  /// record_id → publish target. Built from the JSON ``subscriptions``
  /// array. Read-only after construction.
  std::unordered_map<std::string, PublishTarget> targets_;

  // ── State opened in on_start / closed in on_stop ──────────────────────────
  /// One Adamo session per observer instance; shared across joint-state topics.
  /// Null when no joint-state subscription is configured.
  std::unique_ptr<adamo::Session> session_;
  /// LatestPublisher per fully-qualified topic name. Several record_ids may
  /// share one publisher if they target the same topic.
  std::unordered_map<std::string, std::unique_ptr<trossen_adamo::LatestPublisher>> publishers_;

  /// One Adamo Robot per observer; opened only when at least one camera
  /// subscription exists. ``adamo::Robot::run()`` consumes the handle, so the
  /// pointer is reset to null once the run-thread is spawned — kept here only
  /// for setup-time ``video()`` calls before the move.
  std::unique_ptr<adamo::Robot> adamo_robot_;
  /// Background thread driving ``adamo::Robot::run()`` (a blocking call).
  /// Detached at on_stop because the SDK exposes no graceful stop hook; the
  /// thread therefore lives until process exit. This matches upstream
  /// trossen_adamo's RealSenseStreamer.
  std::thread adamo_robot_thread_;
  /// VideoTrack per fully-qualified track key (``"<robot>/<track_name>"``).
  /// Multiple record_ids may share a track when they all feed the same
  /// camera stream; we deduplicate at on_start.
  std::unordered_map<std::string, std::unique_ptr<adamo::VideoTrack>> video_tracks_;

  /// Scratch buffer reused across BGRA conversions to avoid heap churn at the
  /// frame rate. Owned by the worker thread (single-threaded dispatch).
  std::vector<std::uint8_t> bgra_scratch_;

  std::atomic<uint64_t> skipped_frames_{0};
};

}  // namespace trossen::observer

#endif  // TROSSEN_SDK__OBSERVER__ADAMO_OBSERVER_HPP_
