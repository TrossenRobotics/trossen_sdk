/**
 * @file trossen_base_component.hpp
 * @brief Holonomic swerve base with lift as a base-velocity teleop follower.
 */

#ifndef TROSSEN_SDK__HW__BASE__TROSSEN_BASE_COMPONENT_HPP_
#define TROSSEN_SDK__HW__BASE__TROSSEN_BASE_COMPONENT_HPP_

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "trossen_base/trossen_base.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"

namespace trossen::hw::base {

/**
 * @brief Rivet's mobile base: holonomic swerve drive plus a vertical lift.
 *
 * A `teleop::BaseSpaceTeleop` follower, so any base-space leader can drive it.
 * Unlike the differential-drive SLATE it honours all four axes of the
 * `base_axis` layout: `kLinear` and `kLateral` map to the swerve drive's two
 * translational degrees of freedom and `kLift` to the vertical actuator.
 *
 * The driver needs periodic servicing to keep the wheel modules under command,
 * which does not happen on the teleop write path — a leader that stops writing
 * must not stall the base mid-motion. So the component owns a thread ticking
 * `update_base()` at a fixed rate, independent of whether teleop is running.
 *
 * Expected JSON:
 * @code
 * { "id": "rivet_base", "type": "trossen_base",
 *   "max_linear_mps": 0.6, "max_angular_rps": 1.2,
 *   "max_lift_units_per_s": 8000.0, "ready_timeout_s": 60.0 }
 * @endcode
 */
class TrossenBaseComponent : public HardwareComponent,
                             public teleop::BaseSpaceTeleop {
public:
  explicit TrossenBaseComponent(std::string identifier)
    : HardwareComponent(identifier) {}

  /// Stops the base and joins the update thread. Commanding zero here matters:
  /// the wheels hold their last command until told otherwise, so an
  /// unceremonious teardown mid-motion would leave the robot driving.
  ~TrossenBaseComponent() override;

  /**
   * @brief Connect to the base, wait for it to report ready, start servicing it.
   *
   * @throws std::runtime_error if the base does not become ready within
   *         `ready_timeout_s`, or if any configured limit is not positive.
   */
  void configure(const nlohmann::json& config) override;

  std::string get_type() const override { return "trossen_base"; }

  nlohmann::json get_info() const override;

  /// The underlying driver, for the producer to read odometry from.
  std::shared_ptr<trossen_base::TrossenBase> get_driver() const { return driver_; }

  /// Last velocity actually commanded to the hardware, in `base_axis` order.
  /// Post-clamp and post-e-stop, so it reflects what the base was told rather
  /// than what the leader asked for. The producer records this because the base
  /// reports no measured velocity.
  std::vector<float> last_command() const;

  // ── teleop::BaseSpaceTeleop ──────────────────────────────────────────────

  /// Returns the last commanded velocity as a full `kMaxSize` vector. The base
  /// exposes pose but no measured velocity, so this is a command echo, not a
  /// sensor reading.
  std::vector<float> read() override;

  /// Apply `[linear, angular, lift, lateral]`, clamping each axis to its
  /// configured maximum. Axes absent from a shorter vector are treated as zero.
  /// While the base is e-stopped this commands zero instead of the request.
  void write(const std::vector<float>& cmd) override;

  /// Command zero translation, rotation, and lift. Idempotent.
  void end_teleop() override;

  // ── Emergency stop ───────────────────────────────────────────────────────

  /**
   * @brief Command the base's emergency stop.
   *
   * Zeroes the commanded velocity first, then latches the firmware e-stop, so
   * releasing it later cannot hand the wheels a stale pre-stop velocity.
   *
   * Stops the BASE ONLY. The arms are driven through libtrossen_arm on a
   * separate path and keep doing whatever they were doing; safing them is the
   * caller's job. A caller that stops the base and leaves the arms running is
   * the dangerous case this comment exists to prevent.
   *
   * Not a substitute for the physical e-stop, which cuts power. This one
   * travels over the same link as everything else and cannot stop anything if
   * that link is down.
   *
   * @return true if the command was sent.
   */
  bool emergency_stop();

  /**
   * @brief Clear latched faults and re-enable the base after an e-stop.
   *
   * Firmware blocks re-enable while a critical fault is latched, so faults are
   * cleared first when one is present. Without this the base stays stopped for
   * the rest of the process's life and the only way back is a restart.
   *
   * @return true if the re-enable command was sent.
   */
  bool recover();

  /// @brief Whether the base currently reports itself emergency-stopped.
  ///
  /// Reads the heartbeat's e-stop bit, which the physical e-stop sets too — so
  /// a true here does not mean *this* software stop is the cause. Present it as
  /// "the base is stopped", not "the software stop fired".
  bool is_e_stopped() const;

  /// @brief Live values for an operator display: battery, pose, faults, e-stop.
  ///
  /// Separate from get_info(), which reports configuration. Safe to call while
  /// the update thread is running.
  nlohmann::json telemetry() const;

private:
  /// Ticks `update_base()` until `update_running_` clears.
  void update_loop();

  /// Send `(linear, lateral, angular, lift)` to the driver and record it as the
  /// last command. Values must already be clamped.
  void send(float linear, float angular, float lift, float lateral);

  std::shared_ptr<trossen_base::TrossenBase> driver_;

  /// Rate at which update_loop() services the driver. Matches the rate the base
  /// was validated at during Rivet bring-up.
  static constexpr double kUpdateHz = 15.0;

  std::thread update_thread_;
  std::atomic<bool> update_running_{false};

  /// Guards the last-command echo, written from the teleop thread and read from
  /// the producer's polling thread.
  mutable std::mutex command_mutex_;
  std::array<float, teleop::base_axis::kMaxSize> last_command_{};

  /// One-shot so an e-stop held down does not log at the teleop rate.
  std::atomic<bool> estop_reported_{false};

  float max_linear_mps_{0.6f};
  float max_angular_rps_{1.2f};
  float max_lift_units_per_s_{8000.0f};
  double ready_timeout_s_{60.0};

  /// Battery percentage at or below which the host should emergency-stop.
  /// Zero disables the check, which is the default: a robot that stops itself
  /// is a surprise, so it is opted into per rig rather than assumed.
  ///
  /// Published through telemetry() rather than acted on here. The component can
  /// halt the base but cannot stop the teleop mirror, home the arms, or end the
  /// session, and a partial stop is worse than none — so the decision belongs to
  /// whoever can carry out all of it.
  float estop_battery_percent_{0.0f};
};

}  // namespace trossen::hw::base

#endif  // TROSSEN_SDK__HW__BASE__TROSSEN_BASE_COMPONENT_HPP_
