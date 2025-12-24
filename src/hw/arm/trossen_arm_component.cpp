/**
 * @file trossen_arm_component.cpp
 * @brief Implementation of TrossenArmComponent
 */

#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include <stdexcept>
#include <string>

namespace trossen::hw::arm {

void TrossenArmComponent::configure(const nlohmann::json& config) {
  // Extract required configuration

  // Parse IP address
  if (!config.contains("ip_address")) {
    throw std::runtime_error("TrossenArmComponent: 'ip_address' is required in config");
  }
  ip_address_ = config.at("ip_address").get<std::string>();

  // Parse model
  if (!config.contains("model")) {
    throw std::runtime_error("TrossenArmComponent: 'model' is required in config");
  }
  model_str_ = config.at("model").get<std::string>();
  trossen_arm::Model model;
  if (model_str_ == "wxai_v0") {
    model = trossen_arm::Model::wxai_v0;
  } else {
    throw std::runtime_error("TrossenArmComponent: Unknown model: " + model_str_);
  }

  // Parse end effector
  if (!config.contains("end_effector")) {
    throw std::runtime_error("TrossenArmComponent: 'end_effector' is required in config");
  }
  trossen_arm::EndEffector end_effector;
  end_effector_str_ = config.at("end_effector").get<std::string>();
  if (end_effector_str_ == "wxai_v0_leader") {
    end_effector = trossen_arm::StandardEndEffector::wxai_v0_leader;
  } else if (end_effector_str_ == "wxai_v0_follower") {
    end_effector = trossen_arm::StandardEndEffector::wxai_v0_follower;
  } else {
    throw std::runtime_error("TrossenArmComponent: Unknown end_effector: " + end_effector_str_);
  }

  // Create and configure driver
  driver_ = std::make_shared<trossen_arm::TrossenArmDriver>();

  try {
    driver_->configure(model, end_effector, ip_address_, true);
  } catch (const std::exception& e) {
    throw std::runtime_error(
      "TrossenArmComponent: Failed to configure driver: " + std::string(e.what()));
  }

  // TODO(lukeschmitt-tr): Can do other configuration like joint characteristics here if needed
}

nlohmann::json TrossenArmComponent::get_info() const {
  nlohmann::json info = {
    {"type", "trossen_arm"},
    {"ip_address", ip_address_},
    {"model", model_str_},
    {"end_effector", end_effector_str_}
  };

  return info;
}

}  // namespace trossen::hw::arm
