/**
 * @file remote_adamo_leader_arm.hpp
 * @brief Virtual joint-space leader arm fed by remote Adamo pubsub.
 *
 * EXPERIMENTAL. Pairs with @ref AdamoObserver on the peer host to realise
 * leader/follower teleop across two machines without any local leader
 * hardware. The remote operator's real leader arm is published by an
 * AdamoObserver as ``<robot>/<arm>/state``; this component subscribes to that
 * topic and presents itself to ``TeleopController`` as a ``JointSpaceTeleop``
 * leader.
 *
 * Role split:
 *   - ``read()``                : return the latest decoded joint positions.
 *   - ``write()``               : no-op (leaders never receive commands).
 *   - ``sync_to_state(local)``  : seed the cache with the local follower's
 *                                  current pose, the fallback returned by
 *                                  ``read()`` until the first wire frame lands.
 *   - ``prepare_for_teleop()``  : open ``adamo::Session``, subscribe to
 *                                  ``<robot>/<arm>/state``, and block until the
 *                                  first frame arrives (readiness signal) or
 *                                  ``ready_timeout_s`` elapses; throws on
 *                                  timeout so ``TeleopController::prepare_teleop``
 *                                  never enters the mirror loop pointed at a
 *                                  silent topic. (AdamoObserver does not publish
 *                                  the upstream ``*_ready`` handshake, so the
 *                                  first state frame is the readiness proxy.)
 *   - ``end_teleop()``          : drain subscriber + close session.
 *
 * Scope of this experimental cut: 7-DOF only (the wire codec hard-codes
 * ``kNumJoints = 7``). Effort feedback from the follower back to a real
 * leader is out of scope here — that would be a sibling component subscribing
 * to ``<robot>/<arm>/effort``.
 */

#ifndef TROSSEN_SDK__HW__ARM__REMOTE_ADAMO_LEADER_ARM_HPP_
#define TROSSEN_SDK__HW__ARM__REMOTE_ADAMO_LEADER_ARM_HPP_

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"

// Forward declarations keep the Adamo SDK + trossen_adamo headers out of the
// public include surface. Concrete definitions are pulled in only by the .cpp.
namespace adamo {
class Session;
}  // namespace adamo
namespace trossen_adamo {
class LatestSubscriber;
}  // namespace trossen_adamo

namespace trossen::hw::arm {

/**
 * @brief Virtual leader arm subscribed to Adamo ``leader_state``.
 *
 * Inherits ``JointSpaceTeleop`` directly so ``as_space_io(Space::Joint)``
 * auto-resolves to ``this``. The Cartesian space is intentionally
 * unsupported — Adamo's wire schema is joint-positions/velocities only.
 */
class RemoteAdamoLeaderArm : public HardwareComponent,
                             public teleop::JointSpaceTeleop {
public:
  explicit RemoteAdamoLeaderArm(std::string identifier)
    : HardwareComponent(std::move(identifier)) {}
  ~RemoteAdamoLeaderArm() override;

  RemoteAdamoLeaderArm(const RemoteAdamoLeaderArm&) = delete;
  RemoteAdamoLeaderArm& operator=(const RemoteAdamoLeaderArm&) = delete;
  RemoteAdamoLeaderArm(RemoteAdamoLeaderArm&&) = delete;
  RemoteAdamoLeaderArm& operator=(RemoteAdamoLeaderArm&&) = delete;

  /**
   * @brief Configure the virtual leader from JSON.
   *
   * Required fields:
   *  - ``robot`` (string) - topic prefix; must match the publishing peer.
   *  - ``arm`` (string)   - leader arm name; the topic is ``<robot>/<arm>/state``
   *                         and must match the peer's publishing ``record_id``.
   *
   * Optional fields:
   *  - ``protocol`` (string)            - ``"quic"`` (default) / ``"udp"`` / ``"tcp"``.
   *  - ``api_key_env`` (string)         - env var holding the API key,
   *                                       default ``"ADAMO_API_KEY"``.
   *  - ``ready_timeout_s`` (number)     - first-frame readiness timeout (s),
   *                                       default 30.
   */
  void configure(const nlohmann::json& config) override;

  std::string get_type() const override { return "remote_adamo_leader"; }
  nlohmann::json get_info() const override;

  // ── TeleopTypeIO (joint space) ────────────────────────────────────────────
  std::vector<float> read() override;
  void write(const std::vector<float>& cmd) override;
  void sync_to_state(const std::vector<float>& state) override;

  // ── TeleopCapable lifecycle ───────────────────────────────────────────────
  void prepare_for_teleop() override;
  void end_teleop() override;

private:
  /// Drain pending samples from the subscriber, decode the newest, and update
  /// ``cached_positions_`` / ``cached_velocities_``. Returns true when at
  /// least one fresh sample was consumed.
  bool poll_latest_();

  // ── Configuration (set in configure(), read-only after) ───────────────────
  std::string robot_;
  std::string arm_;
  std::string protocol_{"quic"};
  std::string api_key_env_{"ADAMO_API_KEY"};
  double ready_timeout_s_{30.0};

  // ── Adamo runtime state (opened in prepare_for_teleop) ────────────────────
  std::unique_ptr<adamo::Session> session_;
  /// Hot-path subscriber: callback-backed, single-slot latest payload.
  std::unique_ptr<trossen_adamo::LatestSubscriber> state_sub_;
  /// Scratch buffer reused across read() calls to avoid heap churn at the
  /// teleop rate.
  std::vector<std::uint8_t> rx_buf_;

  // ── Cached most-recent state ──────────────────────────────────────────────
  std::mutex cache_mu_;
  /// Last decoded joint positions (or the seeded follower pose). Sized to 7
  /// once configured.
  std::vector<float> cached_positions_;
  /// Last decoded joint velocities; defaults to zero until first decode.
  std::vector<float> cached_velocities_;
};

}  // namespace trossen::hw::arm

#endif  // TROSSEN_SDK__HW__ARM__REMOTE_ADAMO_LEADER_ARM_HPP_
