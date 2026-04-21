/**
 * @file trossen_arm_component.cpp
 * @brief Implementation of TrossenArmComponent.
 */

#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace trossen::hw::arm {

void TrossenArmComponent::configure(const nlohmann::json& config) {
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
    is_leader_ = true;
  } else if (end_effector_str_ == "wxai_v0_follower") {
    end_effector = trossen_arm::StandardEndEffector::wxai_v0_follower;
    is_leader_ = false;
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

  // Optional teleop tuning — used by stage() / end_teleop().
  if (config.contains("staged_position")) {
    auto pos = config.at("staged_position").get<std::vector<float>>();
    if (pos.size() != static_cast<size_t>(driver_->get_num_joints())) {
      throw std::runtime_error(
        "TrossenArmComponent: 'staged_position' length (" +
        std::to_string(pos.size()) + ") must match joint count (" +
        std::to_string(driver_->get_num_joints()) + ")");
    }
    staged_position_ = std::move(pos);
  }
  if (config.contains("teleop_moving_time_s")) {
    teleop_moving_time_s_ = config.at("teleop_moving_time_s").get<float>();
    if (teleop_moving_time_s_ < 0.0f || !std::isfinite(teleop_moving_time_s_)) {
      throw std::runtime_error(
        "TrossenArmComponent: 'teleop_moving_time_s' must be non-negative and finite");
    }
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

// ── Space-specific IO ────────────────────────────────────────────────────

std::vector<float> TrossenArmComponent::read_joint() {
  if (!driver_) return {};
  const auto& positions = driver_->get_robot_output().joint.all.positions;
  return std::vector<float>(positions.begin(), positions.end());
}

void TrossenArmComponent::write_joint(const std::vector<float>& cmd) {
  if (!driver_) return;
  if (cmd.size() != static_cast<size_t>(driver_->get_num_joints())) {
    throw std::runtime_error(
      "TrossenArmComponent::write_joint: expected " +
      std::to_string(driver_->get_num_joints()) + " joints, got " +
      std::to_string(cmd.size()));
  }
  std::vector<double> pos_d(cmd.begin(), cmd.end());
  driver_->set_all_positions(pos_d, 0.0, false);
}

std::vector<float> TrossenArmComponent::read_cartesian() {
  if (!driver_) return {};
  const auto& out = driver_->get_robot_output();
  // Layout: [x, y, z, rx, ry, rz, gripper_m]. The first six come from the
  // driver's 6-DoF cartesian pose (translation + axis-angle rotation); the
  // gripper opening is tracked in joint space and appended as a scalar.
  std::vector<float> sample;
  sample.reserve(out.cartesian.positions.size() + 1);
  sample.assign(out.cartesian.positions.begin(), out.cartesian.positions.end());
  sample.push_back(static_cast<float>(out.joint.gripper.position));
  return sample;
}

void TrossenArmComponent::write_cartesian(const std::vector<float>& cmd) {
  if (!driver_ || cmd.size() < 6) return;
  std::array<double, 6> goal;
  std::copy_n(cmd.begin(), 6, goal.begin());
  driver_->set_cartesian_positions(
    goal, trossen_arm::InterpolationSpace::cartesian, 0.0, false);
  // Optional 7th element drives the gripper opening directly.
  if (cmd.size() >= 7) {
    driver_->set_gripper_position(static_cast<double>(cmd[6]), 0.0, false);
  }
}

// ── Shared lifecycle ─────────────────────────────────────────────────────

void TrossenArmComponent::prepare_for_teleop() {
  if (!driver_) return;
  if (is_leader_) {
    // Leader: enable gravity compensation.
    driver_->set_all_modes(trossen_arm::Mode::external_effort);
    std::vector<double> zeros(driver_->get_num_joints(), 0.0);
    driver_->set_all_external_efforts(zeros, 0.0, false);
    return;
  }
  // Follower: enter position mode. The mirror loop drives the follower's
  // joints from here.
  driver_->set_all_modes(trossen_arm::Mode::position);
}

void TrossenArmComponent::end_teleop() {
  if (!driver_) return;
  // Neutralize first (safe regardless of current mode), then gracefully
  // return to rest over the configured trajectory time, then release the
  // driver.
  driver_->set_all_modes(trossen_arm::Mode::idle);
  driver_->set_all_modes(trossen_arm::Mode::position);
  driver_->set_all_positions(
    std::vector<double>(driver_->get_num_joints(), 0.0),
    teleop_moving_time_s_, true);
  driver_->cleanup();
  driver_.reset();
}

void TrossenArmComponent::stage() {
  if (!driver_ || staged_position_.empty()) return;
  driver_->set_all_modes(trossen_arm::Mode::position);
  std::vector<double> pos_d(staged_position_.begin(), staged_position_.end());
  // Non-blocking so multiple arms can stage in parallel.
  driver_->set_all_positions(pos_d, teleop_moving_time_s_, false);
}

REGISTER_HARDWARE(TrossenArmComponent, "trossen_arm")

}  // namespace trossen::hw::arm
