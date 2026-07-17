/**
 * @file bimanual_glide_component.cpp
 * @brief Implementation of BimanualGlideComponent.
 */

#include "trossen_sdk/hw/composite/bimanual_glide_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"



namespace trossen::hw::bimanual_glide {

void BimanualGlideComponent::configure(const nlohmann::json& config) {
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
  if (left_model_str_ == "glide") {
    left_model = trossen_arm::Model::glide_left;
  } else {
    throw std::runtime_error("TrossenArmComponent: Unknown model: " + left_model_str_);
  }
  trossen_arm::Model right_model;
  if (right_model_str_ == "glide") {
    right_model = trossen_arm::Model::glide_right;
  } else {
    throw std::runtime_error("TrossenArmComponent: Unknown model: " + right_model_str_);
  }

  if (config.contains("write_moving_time_s")) {
    write_moving_time_s_ = config.at("write_moving_time_s").get<double>();
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


  if (config.contains("episode_lifecycle_enabled")) {
    episode_lifecycle_enabled_ = config.at("episode_lifecycle_enabled").get<bool>();
  }


  // Configure mode
  right_driver_->set_gripper_mode(trossen_arm::Mode::external_effort);
  // right_driver_->set_all_modes(trossen_arm::Mode::external_effort); // TODO: @schromya
}

nlohmann::json BimanualGlideComponent::get_info() const {
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
std::vector<float> BimanualGlideComponent::read_joint() {
  if (!left_driver_ || !right_driver_) return {};
  const auto& left_positions_ = left_driver_->get_robot_output().joint.all.positions;
  const auto& right_positions_ = right_driver_->get_robot_output().joint.all.positions;
  std::vector<float> positions = std::vector<float>(left_positions_.begin(), left_positions_.end());
  positions.insert(positions.end(), right_positions_.begin(), right_positions_.end());
  return positions;
}

void BimanualGlideComponent::write_joint(const std::vector<float>& cmd) {
  if (!left_driver_ || !right_driver_) return;

  const int EXPECTED_SIZE = 2;  // left and right
  if (cmd.size() != EXPECTED_SIZE) {
    throw std::invalid_argument("BimanualGlideComponent: Invalid command size");
  }

  left_driver_->set_gripper_external_effort(cmd[0], write_moving_time_s_, false);
  right_driver_->set_gripper_external_effort(cmd[1], write_moving_time_s_, false);

}

std::vector<float> BimanualGlideComponent::read_cartesian() {
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
  return sample;
}


// ── Shared lifecycle ─────────────────────────────────────────────────────

void BimanualGlideComponent::on_pre_episode() {
  // Nothing needed for glide
}
void BimanualGlideComponent::prepare_for_teleop(){}
void BimanualGlideComponent::end_teleop(){}
void BimanualGlideComponent::stage(){}



REGISTER_HARDWARE(BimanualGlideComponent,"bimanual_glide")

}  // namespace trossen::hw::bimanual_glide


