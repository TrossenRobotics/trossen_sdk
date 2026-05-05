/**
 * @file so101_arm_component.hpp
 * @brief Hardware component wrapper for SO101 arms.
 */
#ifndef TROSSEN_SDK__HW__ARM__SO101_ARM_COMPONENT_HPP_
#define TROSSEN_SDK__HW__ARM__SO101_ARM_COMPONENT_HPP_
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include "trossen_sdk/hw/arm/so101_arm_driver.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"
namespace trossen::hw::arm {
/**
 * @brief Hardware component for SO101 arms.
 *
 * Wraps SO101ArmDriver and provides JSON configuration. Inherits
 * JointSpaceTeleop to expose joint-space teleop read/write.
 */
class SO101ArmComponent : public HardwareComponent,
                          public teleop::JointSpaceTeleop {
public:
  /**
   * @brief Constructor
   *
   * @param identifier Component identifier
   */
  explicit SO101ArmComponent(std::string identifier) : HardwareComponent(identifier) {}
  ~SO101ArmComponent() override = default;
  /**
   * @brief Configure the arm from JSON
   *
   * Expected JSON format:
   * {
   *   "end_effector": "leader" or "follower",
   *   "port": "/dev/ttyUSB0"
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
  std::string get_type() const override { return "so101_arm"; }
  /**
   * @brief Get human-readable component information
   *
   * @return JSON object with component details
   */
  nlohmann::json get_info() const override;
  /**
   * @brief Get the underlying SO101ArmDriver
   *
   * @return Shared pointer to driver
   */
  std::shared_ptr<SO101ArmDriver> get_driver() const { return driver_; }

  // ── JointSpaceTeleop IO (hot loop) ────────────────────────────────────
  std::vector<float> read() override;
  void write(const std::vector<float>& cmd) override;

  // ── TeleopCapable lifecycle ──────────────────────────────────────────
  void prepare_for_teleop() override;
  void end_teleop() override;
  void stage() override;

private:
  std::shared_ptr<SO101ArmDriver> driver_;
  std::string port_;
  SO101EndEffector end_effector_;

  /// Joint-space pose this arm moves to at session start (via stage()).
  /// Values are in normalized servo units. Length must equal get_num_joints();
  /// validated at configure() time. Empty = no staging.
  std::vector<float> staged_position_;
};
}  // namespace trossen::hw::arm
#endif  // TROSSEN_SDK__HW__ARM__SO101_ARM_COMPONENT_HPP_
