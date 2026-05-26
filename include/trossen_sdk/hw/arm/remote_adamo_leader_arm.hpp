/**
 * @file remote_adamo_leader_arm.hpp
 * @brief Virtual joint-space leader arm fed by remote Adamo pubsub.
 *
 * EXPERIMENTAL. Pairs with @ref AdamoObserver on the peer host to realise
 * leader/follower teleop across two machines without any local leader
 * hardware. The remote operator's real leader arm publishes ``leader_state``;
 * this component subscribes to that topic and presents itself to
 * ``TeleopController`` as a ``JointSpaceTeleop`` leader.
 *
 * Role split:
 *   - ``read()``                : decode the latest ``leader_state`` payload.
 *   - ``write()``               : no-op (leaders never receive commands).
 *   - ``sync_to_state(local)``  : seed the cache with the local follower's
 *                                  current pose so the first tick after
 *                                  ``prepare_for_teleop`` does not jolt the
 *                                  follower towards a stale wire value.
 *   - ``prepare_for_teleop()``  : open ``adamo::Session``, run the ready
 *                                  handshake; throws on timeout so
 *                                  ``TeleopController::prepare_teleop`` never
 *                                  enters the mirror loop without a peer.
 *   - ``end_teleop()``          : drain subscriber + close session.
 *
 * Scope of this experimental cut: 7-DOF only (the wire codec hard-codes
 * ``kNumJoints = 7``). Effort feedback from the follower back to a real
 * leader is out of scope here — that lives in a sibling component that
 * subscribes to ``follower_effort``.
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
class Publisher;
class Subscriber;
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
   *  - ``robot`` (string) - topic prefix; must match peer.
   *
   * Optional fields:
   *  - ``protocol`` (string)            - ``"quic"`` (default) / ``"udp"`` / ``"tcp"``.
   *  - ``api_key_env`` (string)         - env var holding the API key,
   *                                       default ``"ADAMO_API_KEY"``.
   *  - ``ready_timeout_s`` (number)     - handshake timeout, default 30.
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
  std::string protocol_{"quic"};
  std::string api_key_env_{"ADAMO_API_KEY"};
  double ready_timeout_s_{30.0};

  // ── Adamo runtime state (opened in prepare_for_teleop) ────────────────────
  std::unique_ptr<adamo::Session> session_;
  /// Raw subscriber used only by the handshake (the handshake drains samples
  /// synchronously; a callback-backed LatestSubscriber would race with that).
  std::unique_ptr<adamo::Subscriber> handshake_sub_;
  /// Self-side ready publisher (we publish follower_ready, peer publishes
  /// leader_ready). Held so the publisher worker lives until end_teleop().
  std::unique_ptr<adamo::Publisher> self_ready_pub_;
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
  /// True once at least one wire payload has been decoded. Until then,
  /// read() returns the value seeded by sync_to_state().
  bool received_any_{false};
};

}  // namespace trossen::hw::arm

#endif  // TROSSEN_SDK__HW__ARM__REMOTE_ADAMO_LEADER_ARM_HPP_
