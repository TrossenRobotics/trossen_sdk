/**
 * @file teleop_arm_component.hpp
 * @brief Hardware component wrapper for teleoperation with Trossen Robotics arms
 */

#ifndef TROSSEN_SDK__HW__ARM__TELEOP_ARM_COMPONENT_HPP_
#define TROSSEN_SDK__HW__ARM__TELEOP_ARM_COMPONENT_HPP_

#include <memory>
#include <string>

#include "libtrossen_arm/trossen_arm.hpp"

#include "trossen_sdk/hw/hardware_component.hpp"

namespace trossen::hw::arm {

/**
 * @brief Hardware component for teleoperation with Trossen Robotics robot arms
 *
 * Wraps two trossen_arm::TrossenArmDriver instances (leader and follower)
 * and provides JSON configuration for teleoperation scenarios.
 */
class TeleopArmComponent : public HardwareComponent {
public:
  /**
   * @brief Constructor
   *
   * @param identifier Component identifier
   */
  explicit TeleopArmComponent(std::string identifier) : HardwareComponent(identifier) {}
  ~TeleopArmComponent() override = default;

  /**
   * @brief Configure the teleop arm pair from JSON
   *
   * Expected JSON format:
   * {
   *   "leader": {
   *     "ip_address": "192.168.1.100",
   *     "model": "wxai_v0",
   *     "end_effector": "wxai_v0_leader"
   *   },
   *   "follower": {
   *     "ip_address": "192.168.1.101",
   *     "model": "wxai_v0",
   *     "end_effector": "wxai_v0_follower"
   *   }
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
  std::string get_type() const override { return "teleop_arm"; }

  /**
   * @brief Get human-readable component information
   *
   * @return JSON object with component details
   */
  nlohmann::json get_info() const override;

  /**
   * @brief Structure to hold both leader and follower drivers
   */
  struct TeleopDrivers {
    std::shared_ptr<trossen_arm::TrossenArmDriver> leader;
    std::shared_ptr<trossen_arm::TrossenArmDriver> follower;
  };

  /**
   * @brief Get the underlying hardware driver instances
   *
   * @return Structure containing both leader and follower drivers
   */
  TeleopDrivers get_hardware() {
    return {leader_driver_, follower_driver_};
  }

private:
  /// @brief Leader arm driver
  std::shared_ptr<trossen_arm::TrossenArmDriver> leader_driver_;

  /// @brief Follower arm driver
  std::shared_ptr<trossen_arm::TrossenArmDriver> follower_driver_;

  /// @brief Leader arm model
  std::string leader_model_str_;

  /// @brief Leader end effector type
  std::string leader_end_effector_str_;

  /// @brief Leader IP address
  std::string leader_ip_address_;

  /// @brief Follower arm model
  std::string follower_model_str_;

  /// @brief Follower end effector type
  std::string follower_end_effector_str_;

  /// @brief Follower IP address
  std::string follower_ip_address_;
};

}  // namespace trossen::hw::arm

#endif  // TROSSEN_SDK__HW__ARM__TELEOP_ARM_COMPONENT_HPP_
