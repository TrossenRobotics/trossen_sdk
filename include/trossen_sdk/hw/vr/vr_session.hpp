/**
 * @file vr_session.hpp
 * @brief Process-wide shared VR connection for trossen_sdk hardware.
 */

#ifndef TROSSEN_SDK__HW__VR__VR_SESSION_HPP_
#define TROSSEN_SDK__HW__VR__VR_SESSION_HPP_

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>

#include "trossen_vr/vr_manager.hpp"
#include "trossen_vr/vr_types.hpp"

namespace trossen::hw::vr {

/**
 * @brief Logical inputs a VR component can consume from the shared
 * frame stream.
 *
 * Each VR hardware component (arm controller, base joystick, session
 * control, …) claims a non-overlapping subset of these on its
 * configured hand. `VrSession::claim_inputs()` enforces this so
 * conflicts (e.g. two components fighting for the trigger) are caught
 * at configure() time, not as silently-wrong teleop behavior.
 */
enum class VrInput {
  kPose,        ///< 6-DOF hand pose (position + orientation).
  kTrigger,     ///< Index trigger (analog).
  kGrip,        ///< Side/grip button (analog or digital depending on device).
  kThumbstick,  ///< 2-axis thumbstick.
  kButtonA,     ///< A button (right) or X button (left).
  kButtonB,     ///< B button (right) or Y button (left).
  kMenu,        ///< Menu / system button.
};

std::string_view vr_input_name(VrInput input);

/**
 * @brief Process-global shared VR connection.
 *
 * The Meta Quest VR app opens a single WebSocket client connection per host,
 * so every VR hardware component in the same process must share one
 * trossen_vr::VRManager. VrSession is the owner.
 *
 * Ownership model:
 *  - The first component to call `ensure_started(port)` binds the port and
 *    starts the I/O thread. Subsequent calls on the same port increment a
 *    reference count and return immediately.
 *  - Calls on a *different* port while the session is already running throw,
 *    because a single process cannot bind two Quest connections at once.
 *  - Each `ensure_started` pairs with exactly one `release`. When the last
 *    reference goes away the manager is stopped and the port is freed.
 *
 * Thread-safety: all public methods are safe to call from any thread. Reads
 * of the latest frame and connection state are short and take an internal
 * mutex; the underlying trossen_vr::VRManager has its own thread-safe API.
 */
class VrSession {
public:
  /// Access the process-global session instance.
  static VrSession& instance();

  /**
   * @brief Idempotently start the VR connection on `port`.
   *
   * The first caller constructs and starts the underlying VRManager. Each
   * subsequent call increments the reference count, so `release()` must be
   * paired with every successful `ensure_started()` — typically from the
   * destructor of the owning hardware component.
   *
   * @throws std::runtime_error if already running on a different port.
   */
  void ensure_started(std::uint16_t port);

  /**
   * @brief Decrement the reference count; stop VRManager when it hits zero.
   *
   * Safe to call more times than `ensure_started()`; extra calls are no-ops
   * so teardown code does not need to track its own ownership flag.
   */
  void release();

  /// True if the VR app has an active WebSocket connection to this process.
  bool is_quest_connected() const;

  /// Latest VRState frame received from the VR app, or nullopt if the
  /// session is stopped or no frame has arrived yet.
  std::optional<trossen_vr::VRState> latest_frame() const;

  /**
   * @brief Block until the Quest connects or the timeout elapses.
   *
   * Intended for use in `prepare_for_teleop()` to fail fast when the user
   * forgets to launch the Quest app. Polls `is_quest_connected()` at 20 Hz.
   *
   * @return true if a connection was observed before the deadline.
   */
  bool wait_for_connection(std::chrono::milliseconds timeout) const;

  /**
   * @brief Return true once per "start" signal from the VR stream.
   *
   * A start signal is either a fresh `VRCommand::Start` on the most recent
   * frame or a rising edge on the A-button of `controller_hand`. The call
   * is stateful: the signal is consumed, and subsequent calls return false
   * until the next rising edge. Designed to gate a demo's episode loop on
   * an in-VR button so the operator does not need to remove the headset.
   *
   * @param controller_hand "left" or "right" — which A-button to watch.
   *
   * @deprecated Use a `VrSessionControlComponent` attached via
   *             `SessionManager::attach_session_control()` instead. This
   *             method is retained only for the legacy stationary-demo
   *             code path and will be removed once that demo migrates.
   */
  bool consume_start_signal(const std::string& controller_hand = "right");

  /**
   * @brief Reserve a set of logical inputs on one hand for a component.
   *
   * Each VR hardware component calls this in `configure()` to declare
   * what it consumes from the shared frame stream. The session maintains
   * a `(hand, input) -> component_id` map and throws if any requested
   * input is already claimed by a *different* component.
   *
   * Calling `claim_inputs()` a second time with the same `(hand,
   * component_id, inputs)` is idempotent — useful for components that
   * can be reconfigured.
   *
   * @param hand          "left" or "right".
   * @param component_id  Stable identifier of the claiming component
   *                      (typically `HardwareComponent::get_identifier()`).
   * @param inputs        Inputs to claim on that hand.
   *
   * @throws std::runtime_error on conflict or an unrecognized hand.
   */
  void claim_inputs(const std::string& hand,
                    const std::string& component_id,
                    std::initializer_list<VrInput> inputs);

  /**
   * @brief Release all claims held by `component_id`.
   *
   * Safe to call with no outstanding claims. Typically invoked from a
   * VR component's destructor or `end_teleop()`.
   */
  void release_claims(const std::string& component_id);

  VrSession(const VrSession&)            = delete;
  VrSession& operator=(const VrSession&) = delete;
  VrSession(VrSession&&)                 = delete;
  VrSession& operator=(VrSession&&)      = delete;

private:
  VrSession() = default;
  ~VrSession();

  mutable std::mutex                    mutex_;
  std::unique_ptr<trossen_vr::VRManager> manager_;
  std::uint16_t                         port_{0};
  std::size_t                           ref_count_{0};

  /// Rising-edge state for `consume_start_signal`. Keyed per hand so the
  /// left and right A-buttons can be watched independently.
  std::unordered_map<std::string, bool> prev_a_button_;

  /// Last VRState sequence number we consumed a `VRCommand::Start` from.
  /// A fresh Start only fires once per frame sequence, not on repeated reads.
  std::uint64_t last_start_sequence_{0};

  /// `(hand, input) -> component_id` claim table. Populated by
  /// `claim_inputs()`, queried for conflicts, cleared by
  /// `release_claims()` when a component tears down.
  struct ClaimKey {
    std::string hand;
    VrInput     input;
    bool operator==(const ClaimKey& other) const {
      return hand == other.hand && input == other.input;
    }
  };
  struct ClaimKeyHash {
    std::size_t operator()(const ClaimKey& k) const noexcept {
      // Combine the two cheaply; the table stays small (≤ 2 hands ×
      // ~7 inputs), so collision behavior is irrelevant.
      return std::hash<std::string>{}(k.hand) ^
             (std::hash<int>{}(static_cast<int>(k.input)) << 1);
    }
  };
  std::unordered_map<ClaimKey, std::string, ClaimKeyHash> claims_;
};

}  // namespace trossen::hw::vr

#endif  // TROSSEN_SDK__HW__VR__VR_SESSION_HPP_
