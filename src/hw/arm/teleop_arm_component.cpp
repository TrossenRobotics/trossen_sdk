/**
 * @file teleop_arm_component.cpp
 * @brief Implementation of TeleopArmComponent
 */

#include "trossen_sdk/hw/arm/teleop_arm_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include <stdexcept>
#include <string>

namespace trossen::hw::arm {

void TeleopArmComponent::configure(const nlohmann::json& config) {
  // Extract leader configuration
  if (!config.contains("leader")) {
    throw std::runtime_error("TeleopArmComponent: 'leader' configuration is required");
  }
  const auto& leader_config = config.at("leader");

  // Parse leader IP address
  if (!leader_config.contains("ip_address")) {
    throw std::runtime_error("TeleopArmComponent: 'ip_address' is required in leader config");
  }
  leader_ip_address_ = leader_config.at("ip_address").get<std::string>();

  // Parse leader model
  if (!leader_config.contains("model")) {
    throw std::runtime_error("TeleopArmComponent: 'model' is required in leader config");
  }
  leader_model_str_ = leader_config.at("model").get<std::string>();
  trossen_arm::Model leader_model;
  if (leader_model_str_ == "wxai_v0") {
    leader_model = trossen_arm::Model::wxai_v0;
  } else {
    throw std::runtime_error("TeleopArmComponent: Unknown leader model: " + leader_model_str_);
  }

  // Parse leader end effector
  if (!leader_config.contains("end_effector")) {
    throw std::runtime_error("TeleopArmComponent: 'end_effector' is required in leader config");
  }
  trossen_arm::EndEffector leader_end_effector;
  leader_end_effector_str_ = leader_config.at("end_effector").get<std::string>();
  if (leader_end_effector_str_ == "wxai_v0_leader") {
    leader_end_effector = trossen_arm::StandardEndEffector::wxai_v0_leader;
  } else if (leader_end_effector_str_ == "wxai_v0_follower") {
    leader_end_effector = trossen_arm::StandardEndEffector::wxai_v0_follower;
  } else {
    throw std::runtime_error(
      "TeleopArmComponent: Unknown leader end_effector: " + leader_end_effector_str_);
  }

  // Extract follower configuration
  if (!config.contains("follower")) {
    throw std::runtime_error("TeleopArmComponent: 'follower' configuration is required");
  }
  const auto& follower_config = config.at("follower");

  // Parse follower IP address
  if (!follower_config.contains("ip_address")) {
    throw std::runtime_error("TeleopArmComponent: 'ip_address' is required in follower config");
  }
  follower_ip_address_ = follower_config.at("ip_address").get<std::string>();

  // Parse follower model
  if (!follower_config.contains("model")) {
    throw std::runtime_error("TeleopArmComponent: 'model' is required in follower config");
  }
  follower_model_str_ = follower_config.at("model").get<std::string>();
  trossen_arm::Model follower_model;
  if (follower_model_str_ == "wxai_v0") {
    follower_model = trossen_arm::Model::wxai_v0;
  } else {
    throw std::runtime_error(
      "TeleopArmComponent: Unknown follower model: " + follower_model_str_);
  }

  // Parse follower end effector
  if (!follower_config.contains("end_effector")) {
    throw std::runtime_error(
      "TeleopArmComponent: 'end_effector' is required in follower config");
  }
  trossen_arm::EndEffector follower_end_effector;
  follower_end_effector_str_ = follower_config.at("end_effector").get<std::string>();
  if (follower_end_effector_str_ == "wxai_v0_leader") {
    follower_end_effector = trossen_arm::StandardEndEffector::wxai_v0_leader;
  } else if (follower_end_effector_str_ == "wxai_v0_follower") {
    follower_end_effector = trossen_arm::StandardEndEffector::wxai_v0_follower;
  } else {
    throw std::runtime_error(
      "TeleopArmComponent: Unknown follower end_effector: " + follower_end_effector_str_);
  }

  // Create and configure leader driver
  leader_driver_ = std::make_shared<trossen_arm::TrossenArmDriver>();
  try {
    leader_driver_->configure(leader_model, leader_end_effector, leader_ip_address_, true);
  } catch (const std::exception& e) {
    throw std::runtime_error(
      "TeleopArmComponent: Failed to configure leader driver: " + std::string(e.what()));
  }

  // Create and configure follower driver
  follower_driver_ = std::make_shared<trossen_arm::TrossenArmDriver>();
  try {
    follower_driver_->configure(follower_model, follower_end_effector, follower_ip_address_, true);
  } catch (const std::exception& e) {
    throw std::runtime_error(
      "TeleopArmComponent: Failed to configure follower driver: " + std::string(e.what()));
  }
}

nlohmann::json TeleopArmComponent::get_info() const {
  nlohmann::json info = {
    {"type", "teleop_arm"},
    {"leader", {
      {"ip_address", leader_ip_address_},
      {"model", leader_model_str_},
      {"end_effector", leader_end_effector_str_}
    }},
    {"follower", {
      {"ip_address", follower_ip_address_},
      {"model", follower_model_str_},
      {"end_effector", follower_end_effector_str_}
    }}
  };

  return info;
}

REGISTER_HARDWARE(TeleopArmComponent, "teleop_arm")

}  // namespace trossen::hw::arm
