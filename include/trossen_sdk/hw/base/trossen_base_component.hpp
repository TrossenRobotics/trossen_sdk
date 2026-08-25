/**
 * @file trossen_base_component.hpp
 * @brief Holonomic swerve base with lift as a base-velocity teleop follower.
 */

#ifndef TROSSEN_SDK__HW__BASE__TROSSEN_BASE_COMPONENT_HPP_
#define TROSSEN_SDK__HW__BASE__TROSSEN_BASE_COMPONENT_HPP_

#include <array>
#include <atomic>
#include <cstdint>
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
 * ### Four different things are called `trossen_base`
 *
 * The name is kept for compatibility with shipped rig configs, so the
 * disambiguation lives here instead:
 *
 * | `trossen_base` | is |
 * | --- | --- |
 * | `#include "trossen_base/trossen_base.hpp"` | the external drivetrain library |
 * | the CMake target and FetchContent name | that same library, as a dependency |
 * | this class | an SDK *consumer* of that library |
 * | `"type": "trossen_base"` in config | the selector for this component, and the MCAP stream key |
 *
 * This component is not the library. It wraps it.
 *
 * ### It is not the SLATE base either
 *
 * Trossen ships two mobile bases and the names do not distinguish them:
 *
 * - **`trossen_base`** (this one) — the Rivet's **holonomic swerve** base with a
 *   vertical lift. Honours all four `base_axis` axes.
 * - **`slate_base`** — the other one, **differential drive**, no lift. Honours
 *   `kLinear` and `kAngular`; it physically cannot strafe.
 *
 * A config that names the wrong one connects to hardware that is not there.
 *
 * ### `TROSSEN_ENABLE_RIVET` gates the library, not the robot
 *
 * That option means "is the `trossen_base` library available to link against",
 * which is why only this component sits behind it. The Glide input layer is
 * deliberately **not** gated: it needs `libtrossen_arm` and nothing else, so a
 * Workbench builds and runs the whole handle path with the option off.
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
 * ### That servicing thread is why `command_timeout_ms` exists
 *
 * The wheels hold their last commanded velocity, and the servicing thread
 * re-asserts it every tick — so a base told to drive keeps driving until
 * something says otherwise, whether or not anything is still commanding it. A
 * mirror loop that dies, a session that ends without `end_teleop()`, a host
 * process wedged on something else: all of them leave a moving robot with
 * nobody at the controls, and none of them are visible from here.
 *
 * So the servicing thread also checks how old the last accepted command is,
 * and commands a standstill once it exceeds `command_timeout_ms`. The check
 * arms on the FIRST `write()` — before that there is nothing to go stale, and a
 * caller that configures the base without ever driving it (a diagnostic, a
 * hardware test) should not be told its link is dead.
 *
 * This is deliberately independent of `TeleopController`'s leader watchdog.
 * That one detects a leader that stopped answering and needs the host to react;
 * this one is local, needs nobody, and holds whatever else fails.
 *
 * Expected JSON:
 * @code
 * { "id": "rivet_base", "type": "trossen_base",
 *   "max_linear_mps": 0.6, "max_angular_rps": 1.2,
 *   "max_lift_units_per_s": 8000.0, "ready_timeout_s": 60.0,
 *   "home_on_configure": true, "command_timeout_ms": 500.0 }
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
   * @brief Connect to the base, wait for it to report ready, start servicing it,
   *        and (unless `home_on_configure` is false) home the swerve modules.
   *
   * @throws std::runtime_error if the base does not become ready within
   *         `ready_timeout_s`, if homing is requested and does not complete, or
   *         if any configured limit is not positive.
   */
  void configure(const nlohmann::json& config) override;

  /**
   * @brief Re-zero the swerve modules against their hall sensors.
   *
   * Called from configure() when `home_on_configure` is true, which is the
   * default, so a session never starts against a stale zero. The base does
   * self-home at power-on, but a pivot can be nudged by hand or lose its
   * reference to a fault afterwards, and nothing else rechecks it for the rest
   * of the machine's uptime.
   *
   * Blocks until the firmware confirms. The base answers in a second or two in
   * the normal case; the driver waits up to 120s before giving up.
   *
   * `home_on_configure: false` exists for callers that connect to the base for a
   * reason other than driving it -- a diagnostic, a hardware test -- where the
   * mechanical re-home is tens of seconds spent for nothing, and the caller
   * disconnects again without ever commanding a wheel.
   *
   * KEEP IT ON for anything that then moves the base. Skipping it means the
   * modules run on whatever zero they last established, and a pivot that has
   * drifted will translate a commanded heading into the wrong actual heading --
   * a robot that drives off at an angle, not one that refuses to drive.
   *
   * In particular: do NOT set it false for a recording session on the grounds
   * that a hardware test homed a moment ago. It is the obvious next saving --
   * homing dominates what is left of a Rivet's bring-up -- and it is not safe
   * as things stand, because "the modules are still homed" is an assumption
   * nothing can check. There is no query for zero validity, so a pivot nudged
   * between the two bring-ups is undetectable, and the session that inherits it
   * drives wrong with no error anywhere. Making this safe means exposing that
   * query from the driver first, not reasoning about how recently we homed.
   *
   * @throws std::runtime_error if the base does not confirm homing.
   */
  void home_modules();

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
  ///
  /// Also stamps the command clock the freshness watchdog measures against, so
  /// a rejected write (too short a vector) does NOT count as a live link.
  void write(const std::vector<float>& cmd) override;

  /// Command zero translation, rotation, and lift. Idempotent.
  void end_teleop() override;

private:
  /// Ticks `update_base()` until `update_running_` clears, enforcing command
  /// freshness on the way.
  void update_loop();

  /// Command a standstill if the last accepted command is older than
  /// `command_timeout_ms_`. Called once per servicing tick, from that thread.
  ///
  /// No-op while the watchdog is disabled or has never seen a write. Zero is
  /// re-asserted on every stale tick rather than only on the transition, so
  /// nothing that pokes the driver behind our back can restart the wheels while
  /// the link is down.
  void enforce_command_freshness();

  /// Return the watchdog to its unarmed state, for the paths that stop the base
  /// on purpose. Not a way to switch the check off — the next write() re-arms.
  void disarm_command_watchdog();

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

  /// steady_clock nanoseconds at the last accepted write(). Zero means the
  /// watchdog is UNARMED — no command has ever arrived, so none can be late.
  /// Written by whatever thread drives teleop, read by the servicing thread.
  std::atomic<std::int64_t> last_command_ns_{0};

  /// Whether the watchdog is currently holding the base stopped. Drives the
  /// one-shot log lines; the age comparison, not this flag, is
  /// what decides to zero the wheels, so a lost update cannot leave the base
  /// driving.
  std::atomic<bool> command_stale_{false};

  /// How long the base may go without a fresh command before it stops itself,
  /// in milliseconds. 0 disables the check.
  ///
  /// 500 ms by default, against a teleop mirror that writes at 1 kHz — three
  /// orders of magnitude of headroom, so this cannot fire on a merely busy
  /// host, and at the 0.6 m/s default limit it bounds an unattended run to
  /// ~30 cm. Detection granularity is one servicing tick (`kUpdateHz`).
  ///
  /// A caller that commands a velocity and then sleeps longer than this will
  /// now be stopped. That is the intended reading of a velocity command with
  /// nobody refreshing it; a caller that wants sustained motion must keep
  /// writing, as the teleop mirror does.
  double command_timeout_ms_{500.0};

  float max_linear_mps_{0.6f};
  float max_angular_rps_{1.2f};
  float max_lift_units_per_s_{8000.0f};
  double ready_timeout_s_{60.0};

  /// Whether configure() re-zeros the swerve modules. Defaults to true so an
  /// existing config behaves exactly as it did before this knob existed; see
  /// home_modules() for when turning it off is legitimate and when it is not.
  bool home_on_configure_{true};
};

}  // namespace trossen::hw::base

#endif  // TROSSEN_SDK__HW__BASE__TROSSEN_BASE_COMPONENT_HPP_
