/**
 * @file teleop_controller.hpp
 * @brief Teleop controller that mirrors leader joint positions to a follower.
 *
 * The controller takes two TeleopCapable instances (leader and optional follower)
 * and runs a high-rate control loop that reads joint positions from the leader and
 * writes them to the follower. The follower may be nullptr for leader-only setups
 * (e.g. UMI-style handheld capture).
 *
 * Capability is enforced at compile time: passing a non-teleop HardwareComponent
 * (a camera, mobile base, etc.) is a type error rather than a runtime exception.
 * Use trossen::hw::teleop::as_capable<TeleopCapable>(hw) at wiring sites to obtain
 * the typed pointer.
 *
 * Recording is handled separately by existing arm producers — this controller only
 * manages the real-time mirroring loop.
 *
 * Usage:
 * @code
 *   auto leader_hw = HardwareRegistry::create("trossen_arm", "leader", cfg);
 *   auto follower_hw = HardwareRegistry::create("trossen_arm", "follower", cfg);
 *
 *   auto leader = teleop::as_capable<teleop::TeleopCapable>(leader_hw);
 *   auto follower = teleop::as_capable<teleop::TeleopCapable>(follower_hw);
 *
 *   TeleopController::Config tc{.control_rate_hz = 1000.0f};
 *   TeleopController ctrl(leader, follower, tc);
 *
 *   ctrl.start();   // spawns mirroring thread
 *   // ... recording happens via SessionManager ...
 *   ctrl.stop();    // joins thread, cleans up hardware
 * @endcode
 */

#ifndef TROSSEN_SDK__HW__TELEOP__TELEOP_CONTROLLER_HPP
#define TROSSEN_SDK__HW__TELEOP__TELEOP_CONTROLLER_HPP

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "trossen_sdk/hw/teleop/teleop_capable.hpp"

namespace trossen::hw::teleop {

class TeleopController {
public:
  struct Config {
    /// Control loop rate in Hz (how fast leader positions are mirrored to follower)
    float control_rate_hz{1000.0f};

    /// Optional joint index remapping from leader to follower.
    /// If empty, positions are passed through directly.
    /// mapping[i] = leader joint index that maps to follower joint i.
    std::vector<int> joint_mapping;
  };

  /**
   * @brief Construct a teleop controller
   *
   * @param leader Teleop-capable component to read joint positions from
   * @param follower Teleop-capable component to write positions to (nullptr for leader-only)
   * @param config Controller configuration
   * @throws std::invalid_argument if leader is null
   */
  TeleopController(
    std::shared_ptr<TeleopCapable> leader,
    std::shared_ptr<TeleopCapable> follower,
    Config config);

  ~TeleopController();

  // Non-copyable, non-movable (owns a thread)
  TeleopController(const TeleopController&) = delete;
  TeleopController& operator=(const TeleopController&) = delete;

  /**
   * @brief Start the mirroring loop
   *
   * Calls prepare_for_follower() then prepare_for_leader() on the hardware,
   * then spawns a thread that continuously mirrors positions at the configured rate.
   * No-op if already running.
   */
  void start();

  /**
   * @brief Stop the mirroring loop
   *
   * Joins the control thread, then calls cleanup_teleop() on both components.
   * No-op if not running.
   */
  void stop();

  /**
   * @brief Check if the control loop is running
   */
  bool is_running() const { return running_.load(); }

private:
  void control_loop();

  std::shared_ptr<TeleopCapable> leader_;
  std::shared_ptr<TeleopCapable> follower_;
  Config cfg_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace trossen::hw::teleop

#endif  // TROSSEN_SDK__HW__TELEOP__TELEOP_CONTROLLER_HPP
