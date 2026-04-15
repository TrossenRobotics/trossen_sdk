/**
 * @file keyboard_teleop_component.hpp
 * @brief Virtual cartesian-space teleop leader driven by WASD keyboard input.
 */

#ifndef TROSSEN_SDK__HW__INPUT__KEYBOARD_TELEOP_COMPONENT_HPP_
#define TROSSEN_SDK__HW__INPUT__KEYBOARD_TELEOP_COMPONENT_HPP_

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"

namespace trossen::hw::input {

/**
 * @brief Cartesian-space teleop leader driven by keyboard input.
 *
 * Acts as a virtual leader for TeleopController: holds a 6-DOF pose
 * `[x, y, z, rx, ry, rz]` initialized from JSON config and updated by a
 * background thread that polls stdin for WASDRF translation deltas.
 *
 * Key bindings:
 *   - W / S : +X / -X  (forward / backward)
 *   - A / D : -Y / +Y  (left / right)
 *   - R / F : +Z / -Z  (raise / fall)
 *
 * Pair this with any cartesian-capable follower (e.g. TrossenArmComponent)
 * via the standard teleop config, with `"space": "cartesian"` on the pair.
 *
 * Note: the poll thread reads stdin directly. If used alongside the session
 * manager's interactive reset countdown, set `session.reset_duration: 0` to
 * avoid both threads racing for stdin bytes.
 */
class KeyboardTeleopComponent : public HardwareComponent,
                                public teleop::CartesianSpaceTeleop {
public:
  /**
   * @brief Constructor
   *
   * @param identifier Component identifier
   */
  explicit KeyboardTeleopComponent(std::string identifier)
    : HardwareComponent(std::move(identifier)) {}
  ~KeyboardTeleopComponent() override;

  /**
   * @brief Configure from JSON.
   *
   * Expected JSON format:
   * {
   *   "initial_pose":     [0.3, 0.0, 0.4, 0.0, 0.0, 0.0],  // optional, fallback
   *   "max_velocity_m_s": 0.05,                             // optional, default 5cm/s
   *   "key_timeout_ms":   120                               // optional, default 120ms
   * }
   *
   * `initial_pose` is the starting 6-DOF cartesian pose [x, y, z, rx, ry, rz]
   * used in leader-only setups. When the keyboard is paired with a follower
   * via the teleop config, the controller calls `sync_to_state` at session
   * start with the follower's actual pose, overriding `initial_pose`. If
   * neither is available the pose defaults to all zeros.
   *
   * `max_velocity_m_s` is the translation speed applied while a direction
   * key is held. Pose advances by `velocity * dt` on every read(), giving
   * smooth motion at the mirror rate (no per-keypress jumps).
   *
   * `key_timeout_ms` is how long after the last keypress an axis stays
   * "active". When the OS key-repeat stream stops, velocity decays to zero
   * after this window. Defaults to slightly above one OS auto-repeat
   * interval (~33ms at 30Hz repeat rate) for a clean stop on key release.
   *
   * @param config JSON configuration object
   * @throws std::runtime_error if configuration fails
   */
  void configure(const nlohmann::json& config) override;

  std::string get_type() const override { return "keyboard_teleop"; }
  nlohmann::json get_info() const override;

  // ── CartesianSpaceTeleop hot-loop ──────────────────────────────────────
  std::vector<float> read() override;
  void write(const std::vector<float>& cmd) override { (void)cmd; }

  /// Auto-align the virtual pose to the follower's actual cartesian pose at
  /// session start. Overrides any `initial_pose` from JSON when a follower
  /// is paired in the teleop config — eliminates the need to hand-tune the
  /// JSON to match the staged follower pose.
  void sync_to_state(const std::vector<float>& state) override;

  // ── TeleopCapable lifecycle ────────────────────────────────────────────
  // stage() is a no-op (no physical hardware to move).
  void prepare_for_teleop() override;
  void end_teleop() override;

private:
  void poll_loop();

  /// Per-axis "this direction is active until time T" state. Each detected
  /// keypress refreshes the deadline; while the deadline lies in the future
  /// the axis contributes `velocity_m_s` to read()'s pose integration.
  struct AxisState {
    float velocity_m_s{0.0f};       ///< signed velocity (+max / -max / 0)
    std::chrono::steady_clock::time_point active_until{};
  };

  /// Current 6-DOF pose [x, y, z, rx, ry, rz] in meters / radians.
  /// Mutated by both poll_loop() (sets axis velocities) and read() (integrates).
  std::array<float, 6> pose_{};
  mutable std::mutex pose_mutex_;

  /// Active-axis state for X, Y, Z translation (rotation not implemented yet).
  AxisState x_axis_, y_axis_, z_axis_;

  /// Last read() timestamp — used to compute dt for velocity integration.
  std::chrono::steady_clock::time_point last_read_time_{};

  /// Pose used at prepare_for_teleop() time. Read from JSON.
  std::array<float, 6> initial_pose_{};

  /// Translation speed applied while a direction key is held (m/s).
  float max_velocity_m_s_{0.05f};

  /// How long after the last keypress an axis stays active.
  std::chrono::milliseconds key_timeout_{120};

  std::thread poll_thread_;
  std::atomic<bool> running_{false};
};

}  // namespace trossen::hw::input

#endif  // TROSSEN_SDK__HW__INPUT__KEYBOARD_TELEOP_COMPONENT_HPP_
