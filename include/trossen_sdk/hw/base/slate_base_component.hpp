/**
 * @file slate_base_component.hpp
 * @brief Hardware component wrapper for SLATE mobile base
 */

#ifndef TROSSEN_SDK__HW__BASE__SLATE_BASE_COMPONENT_HPP_
#define TROSSEN_SDK__HW__BASE__SLATE_BASE_COMPONENT_HPP_

#include <memory>
#include <string>
#include <vector>

#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"
#include "trossen_slate/trossen_slate.hpp"

namespace trossen::hw::base {

/**
 * @brief Hardware component for SLATE mobile base
 *
 * Wraps the TrossenSlate driver and provides JSON configuration. Implements
 * `teleop::BaseSpaceTeleop` so the base can act as a teleop follower: any
 * base-space leader sends `[linear_mps, angular_rps]` velocity commands that
 * are forwarded to the wheels.
 */
class SlateBaseComponent : public HardwareComponent,
                           public teleop::BaseSpaceTeleop {
public:
  /**
   * @brief Constructor
   *
   * @param identifier Component identifier
   */
  explicit SlateBaseComponent(std::string identifier) : HardwareComponent(identifier) {}
  ~SlateBaseComponent() override = default;

  /**
   * @brief Configure the SLATE base from JSON
   *
   * Expected JSON format:
   * {
   *   "reset_odometry": false,        // Optional: Reset odometry on init (default: false)
   *   "enable_torque": true           // Optional: Enable motor torque (default: true)
   * }
   *
   * @param config JSON configuration object
   * @throws std::runtime_error if configuration fails
   */
  void configure(const nlohmann::json& config) override;

  /**
   * @brief Get the type string for this hardware component
   *
   * @return Type identifier
   */
  std::string get_type() const override { return "slate_base"; }

  /**
   * @brief Get human-readable component information
   *
   * @return JSON object with component details
   */
  nlohmann::json get_info() const override;

  /**
   * @brief Get the underlying TrossenSlate driver
   *
   * @return Shared pointer to driver
   */
  std::shared_ptr<trossen_slate::TrossenSlate> get_driver() const { return driver_; }

  // ── teleop::BaseSpaceTeleop: IO contract ─────────────────────────────────

  /// Return the base's current body-frame velocity as `[linear_mps,
  /// angular_rps]`. Used by the teleop controller to seed leader alignment.
  std::vector<float> read() override;

  /// Apply a `[linear_mps, angular_rps]` velocity command to the base
  /// (follower role). Forwards the two velocity scalars to the SLATE driver,
  /// which applies its own velocity limits. Commands with fewer than two
  /// elements are ignored.
  void write(const std::vector<float>& cmd) override;

  /// Stop the base by commanding zero velocity. Idempotent.
  void end_teleop() override;

private:
  std::shared_ptr<trossen_slate::TrossenSlate> driver_;
  bool reset_odometry_{false};
  bool enable_torque_{true};
};

}  // namespace trossen::hw::base

#endif  // TROSSEN_SDK__HW__BASE__SLATE_BASE_COMPONENT_HPP_
