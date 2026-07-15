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

  // Passive leader (no actuators, e.g. the lightweight Trossen leader): only
  // streams joint positions. prepare_for_teleop()/stage()/end_teleop() skip
  // every motion command when this is false.
  if (config.contains("actuated")) {
    actuated_ = config.at("actuated").get<bool>();
  }

  // Optional affine joint remap applied in read_joint(): out[j] = signs[j] *
  // raw[j] + offsets[j]. Used when the leader's joint frame doesn't map 1:1
  // onto the follower (the lightweight leader inverts J3/J4 and offsets J5).
  // Empty = identity; when provided each array must cover every joint.
  const auto njoints = static_cast<size_t>(driver_->get_num_joints());
  if (config.contains("joint_signs")) {
    joint_signs_ = config.at("joint_signs").get<std::vector<float>>();
    if (!joint_signs_.empty() && joint_signs_.size() != njoints) {
      throw std::runtime_error(
        "TrossenArmComponent: 'joint_signs' length (" +
        std::to_string(joint_signs_.size()) + ") must match joint count (" +
        std::to_string(njoints) + ")");
    }
  }
  if (config.contains("joint_offsets")) {
    joint_offsets_ = config.at("joint_offsets").get<std::vector<float>>();
    if (!joint_offsets_.empty() && joint_offsets_.size() != njoints) {
      throw std::runtime_error(
        "TrossenArmComponent: 'joint_offsets' length (" +
        std::to_string(joint_offsets_.size()) + ") must match joint count (" +
        std::to_string(njoints) + ")");
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
      if (!dst.empty() && dst.size() != njoints) {
        throw std::runtime_error(
          std::string("TrossenArmComponent: '") + key + "' length (" +
          std::to_string(dst.size()) + ") must match joint count (" +
          std::to_string(njoints) + ")");
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
      auto limits = driver_->get_joint_limits();
      for (size_t j = 0; j < njoints && j < limits.size(); ++j) {
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
        driver_->set_joint_limits(limits);
      } catch (const std::exception& e) {
        throw std::runtime_error(
          "TrossenArmComponent: Failed to set joint limits: " + std::string(e.what()));
      }
    }
  }

  // Leader-only gripper force feedback: reflect the follower's measured gripper
  // effort back onto this (actuated) gripper via a cubic curve. Off by default;
  // the cubic constants only matter when enabled.
  if (config.contains("gripper_force_feedback")) {
    gripper_force_feedback_ = config.at("gripper_force_feedback").get<bool>();
  }
  if (config.contains("gripper_feedback_leader_max")) {
    gripper_feedback_leader_max_ = config.at("gripper_feedback_leader_max").get<float>();
  }
  if (config.contains("gripper_feedback_follower_max")) {
    gripper_feedback_follower_max_ = config.at("gripper_feedback_follower_max").get<float>();
  }
  if (config.contains("gripper_feedback_offset")) {
    gripper_feedback_offset_ = config.at("gripper_feedback_offset").get<float>();
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

void TrossenArmComponent::apply_joint_remap(std::vector<float>& v, bool derivative) const {
  for (size_t i = 0; i < v.size(); ++i) {
    const float sign = (i < joint_signs_.size()) ? joint_signs_[i] : 1.0f;
    const float offset = (i < joint_offsets_.size()) ? joint_offsets_[i] : 0.0f;
    // Positions are a full affine map; velocities/efforts flip sign with a
    // joint reversal but carry no positional offset.
    v[i] = derivative ? sign * v[i] : sign * v[i] + offset;
  }
}

std::vector<float> TrossenArmComponent::read_joint() {
  if (!driver_) return {};
  const auto& positions = driver_->get_robot_output().joint.all.positions;
  std::vector<float> out(positions.begin(), positions.end());
  // Apply the optional affine remap so a mismatched leader publishes commands
  // already in the follower's joint frame. Empty arrays = identity.
  apply_joint_remap(out);
  return out;
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

std::optional<float> TrossenArmComponent::read_gripper_effort() {
  if (!driver_) return std::nullopt;
  return static_cast<float>(driver_->get_gripper_effort());
}

void TrossenArmComponent::apply_gripper_feedback(float follower_gripper_effort) {
  if (!driver_) return;
  // Cubic curve (from the bilateral reference): more resistance at higher grip
  // efforts, with an offset that keeps the leader gripper open when nothing is
  // grasped. leader = leader_max·norm^3 + offset.
  float norm = 0.0f;
  if (gripper_feedback_follower_max_ != 0.0f) {
    norm = std::abs(follower_gripper_effort) / gripper_feedback_follower_max_;
    // std::abs already guarantees norm >= 0, so only the upper bound can fire.
    norm = std::min(norm, 1.0f);
  }
  const double leader_effort =
    gripper_feedback_leader_max_ * std::pow(norm, 3) + gripper_feedback_offset_;
  // Ramp the rendered effort over 0.2s (linear interpolation) rather than
  // applying it instantly. At the contact boundary the follower's measured
  // effort flips rapidly between no-contact and contact; applying that to the
  // leader instantly (goal_time 0) sets up a limit-cycle oscillation. The 0.2s
  // ramp acts as a rate limiter that damps the chatter — matching the bilateral
  // reference, which uses the same goal_time on this command.
  driver_->set_gripper_external_effort(leader_effort, 0.2, false);
}

void TrossenArmComponent::summon_joint(const std::vector<float>& cmd) {
  if (!driver_) return;
  if (cmd.size() != static_cast<size_t>(driver_->get_num_joints())) {
    throw std::runtime_error(
      "TrossenArmComponent::summon_joint: expected " +
      std::to_string(driver_->get_num_joints()) + " joints, got " +
      std::to_string(cmd.size()));
  }
  // Blocking, time-parameterised move so the follower eases onto the target
  // (the leader's current pose) over teleop_moving_time_s_ before the high-rate
  // mirror loop takes over with instant writes.
  driver_->set_all_modes(trossen_arm::Mode::position);
  std::vector<double> pos_d(cmd.begin(), cmd.end());
  driver_->set_all_positions(pos_d, teleop_moving_time_s_, true);
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
  if (!actuated_) {
    // Passive leader: arm joints stream positions in position mode (harmless —
    // they have no motors). The gripper, however, may be actuated for force
    // feedback: put it in external-effort mode so the mirror loop's reverse
    // channel can render the reflected grip force; otherwise position mode so
    // its opening is reported cleanly for the follower's passthrough.
    driver_->set_all_modes(trossen_arm::Mode::position);
    if (gripper_force_feedback_) {
      driver_->set_gripper_mode(trossen_arm::Mode::external_effort);
      // Seed the resting offset so the gripper holds open before the first tick.
      driver_->set_gripper_external_effort(gripper_feedback_offset_, 0.0, false);
      // Record that the gripper is now in external-effort mode so end_teleop()
      // only releases it when it was actually engaged (the hardware-test path
      // calls end_teleop() without ever calling prepare_for_teleop()).
      gripper_feedback_engaged_ = true;
    } else {
      driver_->set_gripper_mode(trossen_arm::Mode::position);
    }
    return;
  }
  if (is_leader_) {
    // Leader: enable gravity compensation.
    driver_->set_all_modes(trossen_arm::Mode::external_effort);
    std::vector<double> zeros(driver_->get_num_joints(), 0.0);
    driver_->set_all_external_efforts(zeros, 0.0, false);
    return;
  }
  // Follower: arm joints and gripper in position mode (the mirror loop drives
  // them). set_gripper_mode is called explicitly because relying on
  // set_all_modes alone left the gripper inert — it only tracks the leader's
  // opening when explicitly placed in position mode.
  driver_->set_all_modes(trossen_arm::Mode::position);
  driver_->set_gripper_mode(trossen_arm::Mode::position);
}

void TrossenArmComponent::end_teleop() {
  if (!driver_) return;
  if (!actuated_) {
    // Passive leader: arm joints have no actuators to neutralize. If the
    // gripper was actively rendering force feedback, release it (0 N, then
    // idle) so it stops pushing on the operator's hand before the driver is
    // freed. Guard on gripper_feedback_engaged_: end_teleop() can be called
    // without a preceding prepare_for_teleop() (e.g. the hardware-test park
    // step), and commanding external effort on a gripper still in idle mode is
    // a controller error.
    if (gripper_force_feedback_ && gripper_feedback_engaged_) {
      driver_->set_gripper_external_effort(0.0, 0.0, false);
      driver_->set_gripper_mode(trossen_arm::Mode::idle);
      gripper_feedback_engaged_ = false;
    }
    driver_->cleanup();
    driver_.reset();
    return;
  }
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
  if (!actuated_) return;  // passive leader cannot move to a staging pose
  driver_->set_all_modes(trossen_arm::Mode::position);
  std::vector<double> pos_d(staged_position_.begin(), staged_position_.end());
  // Non-blocking so multiple arms can stage in parallel.
  driver_->set_all_positions(pos_d, teleop_moving_time_s_, false);
}

REGISTER_HARDWARE(TrossenArmComponent, "trossen_arm")

}  // namespace trossen::hw::arm
