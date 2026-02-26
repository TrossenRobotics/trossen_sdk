/**
 * @file slate_base_component.cpp
 * @brief Implementation of SlateBaseComponent
 */

#include "trossen_sdk/hw/base/slate_base_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include <stdexcept>
#include <iostream>

namespace trossen::hw::base {

void SlateBaseComponent::configure(const nlohmann::json& config) {
  // Parse optional configuration parameters
  reset_odometry_ = config.value("reset_odometry", false);
  enable_torque_ = config.value("enable_torque", true);
  enable_charging_ = config.value("enable_charging", false);

  // Create driver
  driver_ = std::make_shared<trossen_slate::TrossenSlate>();

  // Initialize the base
  std::string init_result;
  if (!driver_->init_base(init_result, reset_odometry_)) {
    throw std::runtime_error("Failed to initialize SLATE base: " + init_result);
  }
  std::cout << "SLATE base initialized: " << init_result << std::endl;

  // Configure charging
  std::string charging_result;
  if (!driver_->enable_charging(enable_charging_, charging_result)) {
    throw std::runtime_error("Failed to configure charging: " + charging_result);
  }
  std::cout << "SLATE charging configured: " << charging_result << std::endl;

  // Configure motor torque
  std::string torque_result;
  if (!driver_->enable_motor_torque(enable_torque_, torque_result)) {
    throw std::runtime_error("Failed to configure motor torque: " + torque_result);
  }
  std::cout << "SLATE motor torque configured: " << torque_result << std::endl;
}

nlohmann::json SlateBaseComponent::get_info() const {
  nlohmann::json info;
  info["identifier"] = get_identifier();
  info["type"] = get_type();
  info["reset_odometry"] = reset_odometry_;
  info["enable_torque"] = enable_torque_;
  info["enable_charging"] = enable_charging_;

  if (driver_) {
    info["connected"] = true;
    info["charge"] = driver_->get_charge();
    info["voltage"] = driver_->get_voltage();
    info["current"] = driver_->get_current();

    auto vel = driver_->get_vel();
    info["velocity"]["linear"] = vel[0];
    info["velocity"]["angular"] = vel[1];

    auto pose = driver_->get_pose();
    info["pose"]["x"] = pose[0];
    info["pose"]["y"] = pose[1];
    info["pose"]["theta"] = pose[2];
  } else {
    info["connected"] = false;
  }

  return info;
}

// Register the hardware component with the hardware registry
REGISTER_HARDWARE(SlateBaseComponent, "slate_base")

}  // namespace trossen::hw::base
