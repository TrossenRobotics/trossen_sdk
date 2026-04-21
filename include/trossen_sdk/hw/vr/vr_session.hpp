/**
 * @file vr_session.hpp
 * @brief Process-wide shared VR connection for trossen_sdk hardware.
 */

#ifndef TROSSEN_SDK__HW__VR__VR_SESSION_HPP_
#define TROSSEN_SDK__HW__VR__VR_SESSION_HPP_

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include "trossen_vr/vr_manager.hpp"
#include "trossen_vr/vr_types.hpp"

namespace trossen::hw::vr {

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
};

}  // namespace trossen::hw::vr

#endif  // TROSSEN_SDK__HW__VR__VR_SESSION_HPP_
