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
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "trossen_vr/network_manager.hpp"
#include "trossen_vr/vr_types.hpp"

namespace trossen::hw::vr {

/**
 * @brief Logical inputs a VR component can consume from the shared
 * frame stream.
 *
 * Each VR hardware component (`VrArmComponent`, `VrBaseComponent`,
 * `VrSessionControlComponent`, …) claims a non-overlapping subset of these on its
 * configured controller. `VrSession::claim_inputs()` enforces this so
 * conflicts (e.g. two components fighting for the trigger) are caught
 * at configure() time, not as silently-wrong teleop behavior.
 */
enum class VrInput {
  kPose,        ///< 6-DOF hand pose (position + orientation).
  kTrigger,     ///< Index trigger (analog).
  kThumbstick,  ///< 2-axis thumbstick.
  kButtonOne,     ///< A button (right) or X button (left).
  kButtonTwo,     ///< B button (right) or Y button (left).
};

std::string_view vr_input_name(VrInput input);

/**
 * @brief Process-global shared VR connection.
 *
 * A VR headset app opens a single network connection per host, so every
 * VR hardware component in the same process must share one
 * trossen_vr::NetworkManager. VrSession is the owner.
 *
 * Ownership model:
 *  - The first component to call `ensure_started(port)` binds the port and
 *    starts the I/O thread. Subsequent calls on the same port increment a
 *    reference count and return immediately.
 *  - Calls on a *different* port while the session is already running throw,
 *    because a single process cannot bind two VR connections at once.
 *  - Each `ensure_started` pairs with exactly one `release`. When the last
 *    reference goes away the manager is stopped and the port is freed.
 *
 * Thread-safety: all public methods are safe to call from any thread. Reads
 * of the latest frame and connection state are short and take an internal
 * mutex; the underlying trossen_vr::NetworkManager has its own thread-safe API.
 */
class VrSession {
public:
  /// Access the process-global session instance.
  static VrSession& instance();

  /**
   * @brief Idempotently start the VR connection on `port`.
   *
   * The first caller constructs and starts the underlying NetworkManager. Each
   * subsequent call increments the reference count, so `release()` must be
   * paired with every successful `ensure_started()` — typically from the
   * destructor of the owning hardware component.
   *
   * @throws std::runtime_error if already running on a different port.
   */
  void ensure_started(std::uint16_t port);

  /**
   * @brief Decrement the reference count; stop NetworkManager when it hits zero.
   *
   * Safe to call more times than `ensure_started()`; extra calls are no-ops
   * so teardown code does not need to track its own ownership flag.
   */
  void release();

  /// True if the VR headset has an active network connection to this process.
  /// Note: this and `latest_frame()` are independent snapshots — the link can
  /// drop between the two calls, so a frame may be empty right after this
  /// returns true. Callers needing both must tolerate that gap.
  bool is_vr_connected() const;

  /// Latest VRFrame received from the VR app, or nullopt if the
  /// session is stopped or no frame has arrived yet.
  std::optional<trossen_vr::VRFrame> latest_frame() const;

  /**
   * @brief Block until the headset connects or the timeout elapses.
   *
   * Intended for use in `prepare_for_teleop()` to fail fast when the VR
   * headset app is not running. Polls `is_vr_connected()` at 20 Hz.
   *
   * @return true if a connection was observed before the deadline.
   */
  bool wait_for_connection(std::chrono::milliseconds timeout) const;

  /**
   * @brief Reserve a set of logical inputs on one controller type for a component.
   *
   * Each VR hardware component calls this in `configure()` to declare
   * what it consumes from the shared frame stream. The session maintains
   * a `(controller_type, input) -> component_id` map and throws if any
   * requested input is already claimed by a *different* component.
   *
   * Calling `claim_inputs()` a second time with the same `(controller_type,
   * component_id, inputs)` is idempotent — useful for components that
   * can be reconfigured.
   *
   * @param controller_type  "left" or "right".
   * @param component_id  Stable identifier of the claiming component
   *                      (typically `HardwareComponent::get_identifier()`).
   * @param inputs        Inputs to claim on that controller type.
   *
   * @throws std::invalid_argument if controller_type is not "left"/"right"
   *         or component_id is empty.
   * @throws std::runtime_error if an input is already claimed by a different
   *         component.
   */
  void claim_inputs(const std::string& controller_type,
                    const std::string& component_id,
                    std::initializer_list<VrInput> inputs);

  /**
   * @brief Release all claims held by `component_id`.
   *
   * Safe to call with no outstanding claims. Typically invoked from a
   * VR component's destructor or `end_teleop()`.
   */
  void release_claims(const std::string& component_id);

  /**
   * @brief Testing seam: drive VR components with synthetic frames.
   *
   * Puts the session into a test mode where `ensure_started()` does not open a
   * real network connection and `latest_frame()` / `is_vr_connected()` return
   * the injected values instead of the network manager's. This lets the VR
   * hardware components be exercised without a live headset.
   *
   * Call `set_test_frame()` before configuring the components under test.
   * `set_test_connected()` forces just the connection flag (e.g. to simulate a
   * mid-session drop), and `clear_test_frame()` leaves test mode. For tests
   * only — no effect on production code paths, which never call these.
   */
  void set_test_frame(const trossen_vr::VRFrame& frame);
  void set_test_connected(bool connected);
  void clear_test_frame();

  VrSession(const VrSession&)            = delete;
  VrSession& operator=(const VrSession&) = delete;
  VrSession(VrSession&&)                 = delete;
  VrSession& operator=(VrSession&&)      = delete;

private:
  VrSession() = default;
  ~VrSession();

  mutable std::mutex                          mutex_;
  std::unique_ptr<trossen_vr::NetworkManager> manager_;
  std::uint16_t                               port_{0};
  std::size_t                                 ref_count_{0};

  /// Test-mode override (see `set_test_frame()`). When active, the network
  /// manager is bypassed for connection state and frame reads.
  bool                               test_override_{false};
  bool                               test_connected_{false};
  std::optional<trossen_vr::VRFrame> test_frame_;

  /// `(controller_type, input) -> component_id` claim table. Populated by
  /// `claim_inputs()`, queried for conflicts, cleared by
  /// `release_claims()` when a component tears down.
  struct ClaimKey {
    std::string controller_type;
    VrInput     input;
    bool operator==(const ClaimKey& other) const {
      return controller_type == other.controller_type && input == other.input;
    }
  };
  struct ClaimKeyHash {
    std::size_t operator()(const ClaimKey& k) const noexcept {
      // Combine cheaply; table stays small (≤ 2 controller types × ~5 inputs).
      return std::hash<std::string>{}(k.controller_type) ^
             (std::hash<int>{}(static_cast<int>(k.input)) << 1);
    }
  };
  std::unordered_map<ClaimKey, std::string, ClaimKeyHash> claims_;
};

/**
 * @brief RAII lease on the shared VrSession for one hardware component.
 *
 * Acquires a reference (and remembers the component id for claim cleanup) on
 * `acquire()`, and releases the reference plus the component's input claims
 * exactly once on `reset()` / destruction. This makes an accidental
 * double-release impossible, so a component can never close the shared
 * connection while another is still using it. Move-only.
 */
class VrSessionLease {
public:
  VrSessionLease() = default;
  ~VrSessionLease() { reset(); }

  VrSessionLease(const VrSessionLease&)            = delete;
  VrSessionLease& operator=(const VrSessionLease&) = delete;
  VrSessionLease(VrSessionLease&& other) noexcept { *this = std::move(other); }
  VrSessionLease& operator=(VrSessionLease&& other) noexcept {
    if (this != &other) {
      reset();
      component_id_ = std::move(other.component_id_);
      held_         = other.held_;
      other.held_   = false;
    }
    return *this;
  }

  /// Start or join the shared session on `port`, tying the lease to
  /// `component_id` for claim cleanup. Replaces any lease already held.
  void acquire(std::uint16_t port, std::string component_id) {
    reset();
    VrSession::instance().ensure_started(port);
    component_id_ = std::move(component_id);
    held_ = true;
  }

  /// Release the reference and this component's input claims. Idempotent.
  void reset() {
    if (held_) {
      VrSession::instance().release_claims(component_id_);
      VrSession::instance().release();
      held_ = false;
    }
  }

  bool held() const { return held_; }

private:
  std::string component_id_;
  bool        held_{false};
};

}  // namespace trossen::hw::vr

#endif  // TROSSEN_SDK__HW__VR__VR_SESSION_HPP_
