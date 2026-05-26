/**
 * @file adamo_observer.hpp
 * @brief Observer that publishes local JointState records onto Adamo pubsub.
 *
 * EXPERIMENTAL. Pairs with @ref RemoteAdamoLeaderArm on the peer host to
 * realise leader/follower teleop across two machines:
 *
 *   Leader host:    real leader arm + AdamoObserver  →  bus (leader_state)
 *   Follower host:  bus (leader_state) → RemoteAdamoLeaderArm
 *                   real follower arm + AdamoObserver  →  bus (follower_effort)
 *
 * Subscribes to one or more local ``JointStateRecord`` streams and publishes
 * each into Adamo via ``trossen_adamo::wire`` codecs and ``LatestPublisher``.
 *
 * Scope of this experimental cut:
 *   - 7-DOF only (the wire codec hard-codes ``kNumJoints = 7``).
 *   - JointState publish only. Image / Odometry / inbound effort wiring is
 *     stubbed pending the Adamo video bindings + a separate
 *     ``RemoteEffortReceiver`` design.
 */

#ifndef TROSSEN_SDK__OBSERVER__ADAMO_OBSERVER_HPP_
#define TROSSEN_SDK__OBSERVER__ADAMO_OBSERVER_HPP_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "nlohmann/json.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/observer/observer_base.hpp"

// Forward declare to keep Adamo SDK headers out of the public include surface.
// Defined in <adamo/adamo.hpp>, pulled in only by the .cpp.
namespace adamo {
class Session;
}  // namespace adamo
namespace trossen_adamo {
class LatestPublisher;
}  // namespace trossen_adamo

namespace trossen::observer {

/**
 * @brief Per-subscription publish target (what topic to write a record onto).
 *
 * Maps directly onto the trossen_adamo wire schema:
 *  - ``kLeaderState``     -> ``<robot>/trossen/reference/leader_state``,
 *                            encoded via ``wire::encode_state``.
 *  - ``kFollowerEffort``  -> ``<robot>/trossen/reference/follower_effort``,
 *                            encoded via ``wire::encode_efforts``.
 *
 * Resolved from the JSON ``topic`` string on each subscription entry.
 */
enum class AdamoPublishTopic {
  kLeaderState,
  kFollowerEffort,
};

/**
 * @brief Observer that publishes local JointState records onto Adamo pubsub.
 *
 * Lifecycle parallels @ref RerunObserver: ``on_start()`` opens the
 * ``adamo::Session`` and per-topic ``LatestPublisher`` workers, ``on_stop()``
 * drains them. Each subscription's handler runs on the shared observer
 * worker, casts the ``RecordBase`` to ``JointStateRecord``, encodes per the
 * resolved topic, and hands the bytes to the matching ``LatestPublisher``.
 *
 * Publish latency is decoupled from the observer worker via
 * ``LatestPublisher``'s internal thread, so a slow network never stalls the
 * worker (and thus never stalls the producers feeding it).
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
   *    ``record_id``, ``throttle_hz``, and ``topic`` ("leader_state" or
   *    "follower_effort").
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

  /// Configured robot/topic prefix (e.g. ``"wxai"``).
  const std::string& robot() const noexcept { return robot_; }

  /// Records the worker reached but did not publish (wrong record type, joint
  /// count mismatch, encoder threw). Useful for spotting a silently-empty bus.
  uint64_t skipped_frames() const noexcept {
    return skipped_frames_.load(std::memory_order_relaxed);
  }

  /// Per-subscription state resolved at construction time. Public so the
  /// anonymous-namespace ``make_target`` helper in the .cpp can return it.
  struct PublishTarget {
    AdamoPublishTopic topic{AdamoPublishTopic::kLeaderState};
    /// Resolved fully-qualified Adamo topic name (e.g.
    /// ``"wxai/trossen/reference/leader_state"``).
    std::string topic_name;
  };

protected:
  bool on_start() override;
  void on_stop() override;

private:
  /// Worker-thread dispatch entry point captured into each subscription's
  /// handler. Resolves the topic, encodes the record, and hands the bytes off
  /// to the matching ``LatestPublisher``.
  void dispatch_(const std::string& record_id,
                 const std::shared_ptr<data::RecordBase>& rec);

  // ── Configuration (set in ctor, read-only after) ──────────────────────────
  std::string robot_;
  std::string protocol_;
  std::string api_key_env_;

  /// record_id → publish target. Built from the JSON ``subscriptions``
  /// array. Read-only after construction.
  std::unordered_map<std::string, PublishTarget> targets_;

  // ── State opened in on_start / closed in on_stop ──────────────────────────
  /// One Adamo session per observer instance; shared across topics.
  std::unique_ptr<adamo::Session> session_;
  /// LatestPublisher per fully-qualified topic name. Several record_ids may
  /// share one publisher if they target the same topic.
  std::unordered_map<std::string, std::unique_ptr<trossen_adamo::LatestPublisher>> publishers_;

  std::atomic<uint64_t> skipped_frames_{0};
};

}  // namespace trossen::observer

#endif  // TROSSEN_SDK__OBSERVER__ADAMO_OBSERVER_HPP_
