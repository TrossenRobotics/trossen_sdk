/**
 * @file rivet_component.cpp
 * @brief Implementation of RivetComponent.
 */

#include "trossen_sdk/hw/composite/rivet_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>


namespace trossen::hw::rivet {

void RivetComponent::configure(const nlohmann::json& config) {
  // Parse IP address
  if (!config.contains("left_ip_address")) {
    throw std::runtime_error("TrossenArmComponent: 'left_ip_address' is required in config");
  }
  left_ip_address_ = config.at("left_ip_address").get<std::string>();
  if (!config.contains("right_ip_address")) {
    throw std::runtime_error("TrossenArmComponent: 'right_ip_address' is required in config");
  }
  right_ip_address_ = config.at("right_ip_address").get<std::string>();

  // Parse model
  if (!config.contains("left_model")) {
    throw std::runtime_error("TrossenArmComponent: 'left_model' is required in config");
  }
  left_model_str_ = config.at("left_model").get<std::string>();
  if (!config.contains("right_model")) {
    throw std::runtime_error("TrossenArmComponent: 'right_model' is required in config");
  }
  right_model_str_ = config.at("right_model").get<std::string>();

  trossen_arm::Model left_model;
  if (left_model_str_ == "pro") {
    left_model = trossen_arm::Model::pro;
  } else {
    throw std::runtime_error("TrossenArmComponent: Unknown model: " + left_model_str_);
  }
  trossen_arm::Model right_model;
  if (right_model_str_ == "pro") {
    right_model = trossen_arm::Model::pro;
  } else {
    throw std::runtime_error("TrossenArmComponent: Unknown model: " + right_model_str_);
  }

  // Parse end effector
  if (!config.contains("end_effector")) {
    throw std::runtime_error("TrossenArmComponent: 'end_effector' is required in config");
  }
  trossen_arm::EndEffector end_effector;
  end_effector_str_ = config.at("end_effector").get<std::string>();
  if (end_effector_str_ == "wxai_v0_follower") {
    end_effector = trossen_arm::StandardEndEffector::wxai_v0_follower;
  } else {
    throw std::runtime_error("TrossenArmComponent: Unknown end_effector: " + end_effector_str_);
  }

  // Create and configure driver
  left_driver_ = std::make_shared<trossen_arm::TrossenArmDriver>();
  right_driver_ = std::make_shared<trossen_arm::TrossenArmDriver>();

  try {
    left_driver_->configure(left_model, trossen_arm::StandardEndEffector::wxai_v0_leader,
      left_ip_address_, true);
    right_driver_->configure(right_model, trossen_arm::StandardEndEffector::wxai_v0_leader,
      right_ip_address_, true);
  } catch (const std::exception& e) {
    throw std::runtime_error(
      "TrossenArmComponent: Failed to configure driver: " + std::string(e.what()));
  }

  if(left_driver_->get_num_joints() != right_driver_->get_num_joints()) {
    throw std::runtime_error(
      "TrossenArmComponent: Left and right arms must have the same number of joints. (LEFT: "
      + std::to_string(left_driver_->get_num_joints()) + ", RIGHT: "
      + std::to_string(right_driver_->get_num_joints()) + ")");
  }
  njoints_ = static_cast<size_t>(left_driver_->get_num_joints());


  if (config.contains("episode_lifecycle_enabled")) {
    episode_lifecycle_enabled_ = config.at("episode_lifecycle_enabled").get<bool>();
  }

  // Optional teleop tuning — used by stage() / end_teleop().
  if (config.contains("staged_position")) {
    auto pos = config.at("staged_position").get<std::vector<float>>();
    if (pos.size() != njoints_) {
      throw std::runtime_error(
        "TrossenArmComponent: 'staged_position' length (" +
        std::to_string(pos.size()) + ") must match joint count (" + std::to_string(njoints_) + ")");
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

  // Optional per-joint operating limits (position / velocity / effort) and
  // their tolerances. The controller clips commands to these and resets them to
  // firmware defaults on every power cycle, so we re-push them here on each
  // (re)connect. Start from the controller's current limits and override only
  // the fields provided, leaving any unset field at its firmware default.
  {
    auto parse_limit = [&](const char* key, std::vector<float>& dst) {
      if (!config.contains(key)) return;
      dst = config.at(key).get<std::vector<float>>();
      if (!dst.empty() && dst.size() != njoints_) {
        throw std::runtime_error(
          std::string("TrossenArmComponent: '") + key + "' length (" +
          std::to_string(dst.size()) + ") must match joint count (" +
          std::to_string(njoints_) + ")");
      }
    };
    parse_limit("position_min", position_min_);
    parse_limit("position_max", position_max_);
    parse_limit("velocity_max", velocity_max_);
    parse_limit("effort_max", effort_max_);
    parse_limit("position_tolerance", position_tolerance_);
    parse_limit("velocity_tolerance", velocity_tolerance_);
    parse_limit("effort_tolerance", effort_tolerance_);

    if (!position_min_.empty() || !position_max_.empty() ||
        !velocity_max_.empty() || !effort_max_.empty() ||
        !position_tolerance_.empty() || !velocity_tolerance_.empty() ||
        !effort_tolerance_.empty()) {
      for (auto& driver : {left_driver_, right_driver_}) {
        auto limits = driver->get_joint_limits();
        for (size_t j = 0; j < njoints_ && j < limits.size(); ++j) {
          if (!position_min_.empty()) limits[j].position_min = position_min_[j];
          if (!position_max_.empty()) limits[j].position_max = position_max_[j];
          if (!velocity_max_.empty()) limits[j].velocity_max = velocity_max_[j];
          if (!effort_max_.empty()) limits[j].effort_max = effort_max_[j];
          if (!position_tolerance_.empty())
            limits[j].position_tolerance = position_tolerance_[j];
          if (!velocity_tolerance_.empty())
            limits[j].velocity_tolerance = velocity_tolerance_[j];
          if (!effort_tolerance_.empty())
            limits[j].effort_tolerance = effort_tolerance_[j];
        }
        try {
          driver->set_joint_limits(limits);
        } catch (const std::exception& e) {
          throw std::runtime_error(
            "RivetComponent: Failed to set joint limits: " + std::string(e.what()));
        }
      }
    }
  }

  // Configure mode
  right_driver_->set_gripper_mode(trossen_arm::Mode::external_effort);
  left_driver_->set_gripper_mode(trossen_arm::Mode::external_effort);

  // TODO(lukeschmitt-tr): Can do other configuration like joint characteristics here if needed
}

nlohmann::json RivetComponent::get_info() const {
  nlohmann::json info = {
    {"type" , get_type()},
    {"left_ip_address", left_ip_address_},
    {"left_model", left_model_str_},
    {"right_ip_address", right_ip_address_},
    {"right_model", right_model_str_}
  };

  return info;
}

// ── Space-specific IO ────────────────────────────────────────────────────


std::vector<float> RivetComponent::read_joint() {

  if (!left_driver_ || !right_driver_) return {};
  const auto& left_positions = left_driver_->get_robot_output().joint.all.positions;
  std::vector<float> left_out(left_positions.begin(), left_positions.end());

  const auto& right_positions = right_driver_->get_robot_output().joint.all.positions;
  std::vector<float> right_out(right_positions.begin(), right_positions.end());

  std::vector<float> positions(left_out.begin(), left_out.end());
  positions.insert(positions.end(), right_out.begin(), right_out.end());

  // TODO: @schromya add base support
  return positions;
}


void RivetComponent::write_joint(const std::vector<float>& cmd)
{
  if (!left_driver_ || !right_driver_) return;
  size_t expected_cmd_size = static_cast<size_t>(njoints_*2);

  if (cmd.size() != expected_cmd_size) {
    throw std::runtime_error(
      "TrossenArmComponent::write_joint: expected " + std::to_string(expected_cmd_size)
      + " joints, got " + std::to_string(cmd.size()));
  }

  std::vector<double> left_pos(cmd.begin(), cmd.end()-njoints_);
  std::vector<double> right_pos(cmd.begin()+njoints_, cmd.end());

  left_driver_->set_all_positions(left_pos, write_moving_time_s_, false);
  right_driver_->set_all_positions(right_pos, write_moving_time_s_, false);

  // TODO: @schromya add base support
}

std::vector<float> RivetComponent::read_gripper_effort() {
  if (!left_driver_ || !right_driver_)  return {};
  return std::vector<float>({static_cast<float>(left_driver_->get_gripper_effort()),
    static_cast<float>(right_driver_->get_gripper_effort())});
}

void RivetComponent::summon_joint(const std::vector<float>& cmd) {
  if (!left_driver_ || !right_driver_) return;
  size_t expected_cmd_size = static_cast<size_t>(njoints_*2);

  if (cmd.size() != expected_cmd_size) {
    throw std::runtime_error(
      "TrossenArmComponent::write_joint: expected " + std::to_string(expected_cmd_size)
      + " joints, got " + std::to_string(cmd.size()));
  }


  left_driver_->set_all_modes(trossen_arm::Mode::position);
  right_driver_->set_all_modes(trossen_arm::Mode::position);


  std::vector<double> left_pos(cmd.begin(), cmd.end()-njoints_);
  std::vector<double> right_pos(cmd.begin()+njoints_, cmd.end());

  // Blocking, time-parameterised move so the follower eases onto the target
  // (the leader's current pose) over staging_time_s_ before the high-rate
  // mirror loop takes over with instant writes.
  left_driver_->set_all_positions(left_pos, staging_time_s_, true);
  right_driver_->set_all_positions(right_pos, staging_time_s_, true);

}

std::vector<float> RivetComponent::read_cartesian() {
  if (!left_driver_ || !right_driver_) return {};
  const int LEN = 14; // (4 cart + 3 rotation vector + 1 gripper) * 2
  const auto& left_out = left_driver_->get_robot_output();
  const auto& right_out = right_driver_->get_robot_output();

  // Layout: [left_x, left_y, left_z, left_rx, left_ry, left_rz,
  //         left_gripper_m, right_x, right_y, ...]. The first 6 cartesian come from the
  // driver's 6-DoF cartesian pose (translation + rotation vector); the
  // gripper opening is tracked in joint space and appended as a scalar.
  std::vector<float> sample;
  sample.reserve(LEN);
  sample.assign(left_out.cartesian.positions.begin(), left_out.cartesian.positions.end());
  sample.push_back(static_cast<float>(left_out.joint.gripper.position));
  sample.insert(sample.end(), right_out.cartesian.positions.begin(),
    right_out.cartesian.positions.end());
  sample.push_back(static_cast<float>(right_out.joint.gripper.position));

  //TODO: @schromya add base support
  return sample;

}

void RivetComponent::write_cartesian(const std::vector<float>& cmd) {
  if (!left_driver_ || !right_driver_) return;
  const int EXPECTED_LEN = 14; // (4 cart + 3 rotation vector + 1 gripper) * 2
  const int CART_LEN = 6;
  if (EXPECTED_LEN != cmd.size()) return;
  std::array<double, CART_LEN> left_cartesian_goal, right_cartesian_goal;
  std::copy_n(cmd.begin(), CART_LEN, left_cartesian_goal.begin());
  std::copy_n(cmd.begin() + CART_LEN, CART_LEN, right_cartesian_goal.begin());

  left_driver_->set_cartesian_positions(left_cartesian_goal,
    trossen_arm::InterpolationSpace::cartesian, 0.0, false);
  right_driver_->set_cartesian_positions(right_cartesian_goal,
    trossen_arm::InterpolationSpace::cartesian, 0.0, false);

  left_driver_->set_gripper_position(static_cast<double>(cmd[CART_LEN]), 0.0, false);
  right_driver_->set_gripper_position(static_cast<double>(cmd[EXPECTED_LEN-1]), 0.0, false);
}

// ── Shared lifecycle ─────────────────────────────────────────────────────

void RivetComponent::prepare_for_teleop() {
  if (!left_driver_ || !right_driver_) return;

  // Follower: arm joints and gripper in position mode (the mirror loop drives
  // them). set_gripper_mode is called explicitly because relying on
  // set_all_modes alone left the gripper inert — it only tracks the leader's
  // opening when explicitly placed in position mode.
  left_driver_->set_all_modes(trossen_arm::Mode::position);
  left_driver_->set_gripper_mode(trossen_arm::Mode::position);
  right_driver_->set_all_modes(trossen_arm::Mode::position);
  right_driver_->set_gripper_mode(trossen_arm::Mode::position);

  // TODO: @schromya add base support
}

void RivetComponent::end_teleop() {
  if (!left_driver_ || !right_driver_) return;

  // TODO: @schromya add base support

  std::cout << "  [end_teleop] " << get_identifier()
            << ": holding pose, then returning to rest over "
            << staging_time_s_ << "s..." << std::endl;
  // Hold the current pose before resting, so the arm doesn't drop under
  // gravity on Ctrl+C before position control engages. Switch into position
  // mode and immediately command the measured pose (goal_time 0 = zero
  // displacement, since the arm is already there) to seed the position
  // setpoint to where the arm actually is, so it holds. Then drive it to rest
  // over the configured trajectory time.
  const std::vector<float> left_current = read_joint();
  left_driver_->set_all_modes(trossen_arm::Mode::position);
  if (!left_current.empty()) {
    left_driver_->set_all_positions( std::vector<double>(left_current.begin(), left_current.end()),
      0.0, true);
  }
  left_driver_->set_all_positions(std::vector<double>(left_driver_->get_num_joints(), 0.0),
    staging_time_s_, true);
  left_driver_->cleanup();
  left_driver_.reset();


  const std::vector<float> right_current = read_joint();
  right_driver_->set_all_modes(trossen_arm::Mode::position);
  if (!right_current.empty()) {
    right_driver_->set_all_positions( std::vector<double>(right_current.begin(),
    right_current.end()), 0.0, true);
  }
  right_driver_->set_all_positions(std::vector<double>(right_driver_->get_num_joints(), 0.0),
    staging_time_s_, true);
  right_driver_->cleanup();
  right_driver_.reset();


  std::cout << "  [end_teleop] " << get_identifier() << ": done" << std::endl;
}


void RivetComponent::on_pre_episode() {
  // HardwareComponent per-episode hook: re-home this arm before each episode.
  // The SessionManager calls this only when is_episode_lifecycle_enabled() is
  // true, and pauses any teleop mirror around the call, so stage() can drive
  // the arm safely. stage() itself is a no-op when no staged_position is set.
  stage();
}

void RivetComponent::stage() {
  if (!left_driver_ || !right_driver_) return;

  if (staged_position_.empty()) {
    std::cout << "  [stage] " << get_identifier()
              << ": no staged_position configured, skipping" << std::endl;
    return;
  }
  std::cout << "  [stage] " << get_identifier() << ": moving to home over "
            << staging_time_s_ << "s" << std::endl;

  left_driver_->set_all_modes(trossen_arm::Mode::position);
  right_driver_->set_all_modes(trossen_arm::Mode::position);

  std::vector<double> left_pos(staged_position_.begin(), staged_position_.begin());
  std::vector<double> right_pos(staged_position_.begin(), staged_position_.begin());

  // Blocking so the arm reaches home before the caller hands it to teleop
  // (gravity-comp) or starts recording; this mirrors end_teleop()'s rest move.
  left_driver_->set_all_positions(left_pos, staging_time_s_, true);
  right_driver_->set_all_positions(right_pos, staging_time_s_, true);
}

REGISTER_HARDWARE(RivetComponent, "rivet")
}