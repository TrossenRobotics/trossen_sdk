/**
 * @file trossen_arm_component.cpp
 * @brief Implementation of TrossenArmComponent
 */

#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
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

size_t TrossenArmComponent::num_joints() const {
  return driver_ ? static_cast<size_t>(driver_->get_num_joints()) : 0;
}

std::vector<float> TrossenArmComponent::get_joint_positions() {
  if (!driver_) return {};
  auto output = driver_->get_robot_output();
  const auto& positions = output.joint.all.positions;
  return std::vector<float>(positions.begin(), positions.end());
}

void TrossenArmComponent::set_joint_positions(const std::vector<float>& positions) {
  if (!driver_) return;
  std::vector<double> pos_d(positions.begin(), positions.end());
  driver_->set_all_positions(pos_d, 0.0, false);
}

void TrossenArmComponent::prepare_for_leader() {
  if (!driver_) return;
  driver_->set_all_modes(trossen_arm::Mode::external_effort);
  std::vector<double> zeros(driver_->get_num_joints(), 0.0);
  driver_->set_all_external_efforts(zeros, 0.0, false);
}

void TrossenArmComponent::prepare_for_follower(
    const std::vector<float>& initial_positions) {
  if (!driver_) return;
  driver_->set_all_modes(trossen_arm::Mode::position);
  if (!initial_positions.empty()) {
    std::vector<double> pos_d(initial_positions.begin(), initial_positions.end());
    driver_->set_all_positions(pos_d, 2.0, true);
  }
}

void TrossenArmComponent::cleanup_teleop() {
  if (!driver_) return;
  driver_->set_all_modes(trossen_arm::Mode::idle);
}

REGISTER_HARDWARE(TrossenArmComponent, "trossen_arm")

}  // namespace trossen::hw::arm
