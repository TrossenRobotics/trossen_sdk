/**
 * @file so101_arm_component.cpp
 * @brief Implementation of SO101ArmComponent
 */

#include "trossen_sdk/hw/arm/so101_arm_component.hpp"
#include <stdexcept>
namespace trossen::hw::arm {
void SO101ArmComponent::configure(const nlohmann::json& config) {
  // Parse end effector type
  std::string end_effector_str = config.value("end_effector", "follower");
  if (end_effector_str == "leader") {
    end_effector_ = SO101EndEffector::leader;
  } else if (end_effector_str == "follower") {
    end_effector_ = SO101EndEffector::follower;
  } else {
    throw std::runtime_error("Invalid end_effector: " + end_effector_str +
                           " (must be 'leader' or 'follower')");
  }
  // Parse port
  if (!config.contains("port")) {
    throw std::runtime_error("Missing required field: port");
  }
  port_ = config["port"];
  // Create and configure driver
  driver_ = std::make_shared<SO101ArmDriver>();
  if (!driver_->configure(end_effector_, port_)) {
    throw std::runtime_error("Failed to configure SO101ArmDriver on port: " + port_);
  }
  // Connect to hardware
  if (!driver_->connect()) {
    throw std::runtime_error("Failed to connect to SO101 arm on port: " + port_);
  }
}
nlohmann::json SO101ArmComponent::get_info() const {
  nlohmann::json info;
  info["identifier"] = get_identifier();
  info["type"] = get_type();
  info["port"] = port_;
  info["end_effector"] = (end_effector_ == SO101EndEffector::leader) ? "leader" : "follower";
  info["connected"] = driver_ ? driver_->is_connected() : false;
  info["num_joints"] = driver_ ? driver_->get_num_joints() : 0;
  return info;
}
size_t SO101ArmComponent::num_joints() const {
  return driver_ ? driver_->get_num_joints() : 0;
}

std::vector<float> SO101ArmComponent::get_joint_positions() {
  if (!driver_) return {};
  auto positions = driver_->get_joint_positions(true);
  return std::vector<float>(positions.begin(), positions.end());
}

void SO101ArmComponent::set_joint_positions(const std::vector<float>& positions) {
  if (!driver_) return;
  std::vector<double> pos_d(positions.begin(), positions.end());
  driver_->set_joint_positions(pos_d, true);
}

void SO101ArmComponent::prepare_for_leader() {
  // SO101 leader: read-only, no special mode needed
}

void SO101ArmComponent::prepare_for_follower(
    const std::vector<float>& initial_positions) {
  if (!driver_ || initial_positions.empty()) return;
  std::vector<double> pos_d(initial_positions.begin(), initial_positions.end());
  driver_->set_joint_positions(pos_d, true);
}

void SO101ArmComponent::cleanup_teleop() {
  // SO101: no special cleanup needed
}

}  // namespace trossen::hw::arm
