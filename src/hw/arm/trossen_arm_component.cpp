/**
 * @file trossen_arm_component.cpp
 * @brief Implementation of TrossenArmComponent.
 */

#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
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
  if (config.contains("staging_time_s")) {
    staging_time_s_ = config.at("staging_time_s").get<float>();
    if (staging_time_s_ < 0.0f || !std::isfinite(staging_time_s_)) {
      throw std::runtime_error(
        "TrossenArmComponent: 'staging_time_s' must be non-negative and finite");
    }
  }
  if (config.contains("episode_lifecycle_enabled")) {
    episode_lifecycle_enabled_ = config.at("episode_lifecycle_enabled").get<bool>();
  }
  if (config.contains("write_moving_time_s")) {
    write_moving_time_s_ = config.at("write_moving_time_s").get<float>();
    if (write_moving_time_s_ < 0.0f || !std::isfinite(write_moving_time_s_)) {
      throw std::runtime_error(
        "TrossenArmComponent: 'write_moving_time_s' must be non-negative and finite");
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
  driver_->set_all_positions(pos_d, write_moving_time_s_, false);
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
  std::cout << "  [end_teleop] " << get_identifier()
            << ": holding pose, then returning to rest over "
            << staging_time_s_ << "s..." << std::endl;
  // Hold the current pose before resting, so the arm doesn't drop under
  // gravity on Ctrl+C before position control engages. Switch into position
  // mode and immediately command the measured pose (goal_time 0 = zero
  // displacement, since the arm is already there) to seed the position
  // setpoint to where the arm actually is, so it holds. Then drive it to rest
  // over the configured trajectory time.
  const std::vector<float> current = read_joint();
  driver_->set_all_modes(trossen_arm::Mode::position);
  if (!current.empty()) {
    driver_->set_all_positions(
      std::vector<double>(current.begin(), current.end()), 0.0, true);
  }
  driver_->set_all_positions(
    std::vector<double>(driver_->get_num_joints(), 0.0),
    staging_time_s_, true);
  driver_->cleanup();
  driver_.reset();
  std::cout << "  [end_teleop] " << get_identifier() << ": done" << std::endl;
}

void TrossenArmComponent::on_pre_episode() {
  // HardwareComponent per-episode hook: re-home this arm before each episode.
  // The SessionManager calls this only when is_episode_lifecycle_enabled() is
  // true, and pauses any teleop mirror around the call, so stage() can drive
  // the arm safely. stage() itself is a no-op when no staged_position is set.
  stage();
}

void TrossenArmComponent::stage() {
  if (!driver_) return;
  if (staged_position_.empty()) {
    std::cout << "  [stage] " << get_identifier()
              << ": no staged_position configured, skipping" << std::endl;
    return;
  }
  std::cout << "  [stage] " << get_identifier() << ": moving to home over "
            << staging_time_s_ << "s" << std::endl;
  driver_->set_all_modes(trossen_arm::Mode::position);
  std::vector<double> pos_d(staged_position_.begin(), staged_position_.end());
  // Blocking so the arm reaches home before the caller hands it to teleop
  // (gravity-comp) or starts recording; this mirrors end_teleop()'s rest move.
  driver_->set_all_positions(pos_d, staging_time_s_, true);
}

REGISTER_HARDWARE(TrossenArmComponent, "trossen_arm")

}  // namespace trossen::hw::arm
