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


  // Create and configure driver
  left_driver_ = std::make_shared<trossen_arm::TrossenArmDriver>();
  right_driver_ = std::make_shared<trossen_arm::TrossenArmDriver>();

  try {
    left_driver_->configure(left_model, trossen_arm::StandardEndEffector::no_gripper,
      left_ip_address_, true);
    right_driver_->configure(right_model, trossen_arm::StandardEndEffector::no_gripper,
      right_ip_address_, true);
  } catch (const std::exception& e) {
    throw std::runtime_error(
      "TrossenArmComponent: Failed to configure driver: " + std::string(e.what()));
  }


  if (config.contains("episode_lifecycle_enabled")) {
    episode_lifecycle_enabled_ = config.at("episode_lifecycle_enabled").get<bool>();
  }

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
std::vector<float> BimanualGlideComponent::read_joint()
{
  if (!left_driver_ || !right_driver_) return {};
  const auto& left_positions_ = left_driver_->get_robot_output().joint.all.positions;
  const auto& right_positions_ = right_driver_->get_robot_output().joint.all.positions;
  std::vector<float> positions = std::vector<float>(left_positions_.begin(), left_positions_.end());
  positions.insert(positions.end(), right_positions_.begin(), right_positions_.end());
  return positions;
}

void BimanualGlideComponent::write_joint(const std::vector<float>& cmd)
{

}

std::vector<float> BimanualGlideComponent::read_cartesian(){}


// ── Shared lifecycle ─────────────────────────────────────────────────────

void BimanualGlideComponent::on_pre_episode() {}
void BimanualGlideComponent::prepare_for_teleop(){}
void BimanualGlideComponent::end_teleop(){}
void BimanualGlideComponent::stage(){}





}  // namespace trossen::hw::bimanual_glide


