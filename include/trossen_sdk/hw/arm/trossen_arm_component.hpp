/**
 * @file trossen_arm_component.hpp
 * @brief Hardware component wrapper for Trossen Robotics arms
 */

#ifndef TROSSEN_SDK__HW__ARM__TROSSEN_ARM_COMPONENT_HPP_
#define TROSSEN_SDK__HW__ARM__TROSSEN_ARM_COMPONENT_HPP_

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"

#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"

namespace trossen::hw::arm {

/**
 * @brief Hardware component for Trossen Robotics robot arms
 *
 * Wraps trossen_arm::TrossenArmDriver and provides JSON configuration.
 * Implements TeleopCapable so it can act as a teleop leader or follower.
 */
class TrossenArmComponent : public HardwareComponent, public teleop::TeleopCapable {
public:
  /**
   * @brief Constructor
   *
   * @param identifier Component identifier
   */
  explicit TrossenArmComponent(std::string identifier) : HardwareComponent(identifier) {}
  ~TrossenArmComponent() override = default;

  /**
   * @brief Configure the arm from JSON
   *
   * Expected JSON format:
   * {
   *   "ip_address": "192.168.1.100",
   *   "model": "wxai_v0",
   *   "end_effector": "wxai_v0_follower"
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
  std::string get_type() const override { return "trossen_arm"; }

  /**
   * @brief Get human-readable component information
   *
   * @return JSON object with component details
   */
  nlohmann::json get_info() const override;

  /**
   * @brief Get the underlying hardware driver instance
   *
   * @return Shared pointer to driver
   */
  std::shared_ptr<trossen_arm::TrossenArmDriver> get_hardware() { return driver_; }

  // ── TeleopCapable overrides ──────────────────────────────────────────
  std::size_t num_joints() const override;
  std::vector<float> get_joint_positions() override;
  void set_joint_positions(const std::vector<float>& positions) override;
  void prepare_for_leader() override;
  void prepare_for_follower(const std::vector<float>& initial_positions) override;
  void cleanup_teleop() override;

private:
  /// @brief Underlying Trossen arm driver
  std::shared_ptr<trossen_arm::TrossenArmDriver> driver_;

  /// @brief Arm model
  std::string model_str_;

  /// @brief End effector type
  std::string end_effector_str_;

  /// @brief IP address of the arm
  std::string ip_address_;
};

}  // namespace trossen::hw::arm

#endif  // TROSSEN_SDK__HW__ARM__TROSSEN_ARM_COMPONENT_HPP_
