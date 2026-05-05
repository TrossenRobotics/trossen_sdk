/**
 * @file so101_arm_component.cpp
 * @brief Implementation of SO101ArmComponent.
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

  // Optional teleop staging pose — length must match joint count.
  if (config.contains("staged_position")) {
    auto pos = config.at("staged_position").get<std::vector<float>>();
    if (pos.size() != static_cast<size_t>(driver_->get_num_joints())) {
      throw std::runtime_error(
        "SO101ArmComponent: 'staged_position' length (" +
        std::to_string(pos.size()) + ") must match joint count (" +
        std::to_string(driver_->get_num_joints()) + ")");
    }
    staged_position_ = std::move(pos);
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
std::vector<float> SO101ArmComponent::read() {
  if (!driver_) return {};
  auto positions = driver_->get_joint_positions(true);
  return std::vector<float>(positions.begin(), positions.end());
}

void SO101ArmComponent::write(const std::vector<float>& cmd) {
  if (!driver_) return;
  if (cmd.size() != static_cast<size_t>(driver_->get_num_joints())) {
    throw std::runtime_error(
      "SO101ArmComponent::write: expected " +
      std::to_string(driver_->get_num_joints()) + " joints, got " +
      std::to_string(cmd.size()));
  }
  std::vector<double> pos_d(cmd.begin(), cmd.end());
  driver_->set_joint_positions(pos_d, true);
}

void SO101ArmComponent::prepare_for_teleop() {
  // SO101 has no teleop mode to enter — the mirror loop drives it directly.
}

void SO101ArmComponent::end_teleop() {
  // Graceful shutdown: command the zero pose, then release the driver.
  // SO101 does not support trajectory timing; the command is dispatched
  // but does not wait for motion completion.
  if (!driver_) return;
  std::vector<double> zeros(driver_->get_num_joints(), 0.0);
  driver_->set_joint_positions(zeros, true);
  if (driver_->is_connected()) {
    driver_->disconnect();
  }
  driver_.reset();
}

void SO101ArmComponent::stage() {
  if (!driver_ || staged_position_.empty()) return;
  std::vector<double> pos_d(staged_position_.begin(), staged_position_.end());
  driver_->set_joint_positions(pos_d, true);
}

}  // namespace trossen::hw::arm
