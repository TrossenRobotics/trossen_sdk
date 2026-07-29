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
  if (left_model_str_ == "glide_left") {
    left_model = trossen_arm::Model::glide_left;
  } else {
    throw std::runtime_error("TrossenArmComponent: Unknown model: " + left_model_str_);
  }
  trossen_arm::Model right_model;
  if (right_model_str_ == "glide_right") {
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

  // TODO: @schromya Resolve this hack when max gripper position error is fixed in driver
  for (auto& driver : {left_driver_, right_driver_}) {
    auto limits = driver->get_joint_limits();
    if (!limits.empty()) {
      limits.back().position_max = 0.05f;
      try {
        driver->set_joint_limits(limits);
      } catch (const std::exception& e) {
        throw std::runtime_error(
          "BimanualGlideComponent: Failed to set leader gripper position_max: "
          + std::string(e.what()));
      }
    }
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


  // Optional affine joint remap applied in read_joint(): out[j] = signs[j] *
  // raw[j] + offsets[j]. Used when the leader's joint frame doesn't map 1:1
  // onto the follower (the lightweight leader inverts J3/J4 and offsets J5).
  // Empty = identity; when provided each array must cover every joint.
  if (config.contains("joint_signs")) {
    joint_signs_ = config.at("joint_signs").get<std::vector<float>>();
    if (!joint_signs_.empty() && joint_signs_.size() != njoints_) {
      throw std::runtime_error(
        "TrossenArmComponent: 'joint_signs' length (" +
        std::to_string(joint_signs_.size()) + ") must match joint count (" +
        std::to_string(njoints_) + ")");
    }
  }
  if (config.contains("left_joint_offsets")) {
    joint_offsets_left_ = config.at("left_joint_offsets").get<std::vector<float>>();
    if (!joint_offsets_left_.empty() && joint_offsets_left_.size() != njoints_) {
      throw std::runtime_error(
        "TrossenArmComponent: 'left_joint_offsets' length (" +
        std::to_string(joint_offsets_left_.size()) + ") must match joint count (" +
        std::to_string(njoints_) + ")");
    }
  }
  if (config.contains("right_joint_offsets")) {
    joint_offsets_right_ = config.at("right_joint_offsets").get<std::vector<float>>();
    if (!joint_offsets_right_.empty() && joint_offsets_right_.size() != njoints_) {
      throw std::runtime_error(
        "TrossenArmComponent: 'right_joint_offsets' length (" +
        std::to_string(joint_offsets_right_.size()) + ") must match joint count (" +
        std::to_string(njoints_) + ")");
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
            "BimanualGlideComponent: Failed to set joint limits: " + std::string(e.what()));
        }
      }
    }
  }

  // Leader-only gripper force feedback: reflect the follower's measured gripper
  // effort back onto this (actuated) gripper via a cubic curve. Off by default;
  // the cubic constants only matter when enabled.
  if (config.contains("gripper_feedback_leader_max")) {
    gripper_feedback_leader_max_ = config.at("gripper_feedback_leader_max").get<float>();
  }
  if (config.contains("gripper_feedback_follower_max")) {
    gripper_feedback_follower_max_ = config.at("gripper_feedback_follower_max").get<float>();
  }
  if (config.contains("gripper_feedback_offset")) {
    gripper_feedback_offset_ = config.at("gripper_feedback_offset").get<float>();
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
void BimanualGlideComponent::apply_joint_remap(
  std::vector<float>& v, const std::vector<float>& offsets, bool derivative) const {
  for (size_t i = 0; i < v.size(); ++i) {
    const float sign = (i < joint_signs_.size()) ? joint_signs_[i] : 1.0f;
    const float offset = (i < offsets.size()) ? offsets[i] : 0.0f;
    // Positions are a full affine map; velocities/efforts flip sign with a
    // joint reversal but carry no positional offset.
    v[i] = derivative ? sign * v[i] : sign * v[i] + offset;
  }
}

std::vector<float> BimanualGlideComponent::read_joint() {
  if (!left_driver_ || !right_driver_) return {};
  const int LEN = 18; // (7 joints) * 2 + 4 base
  const auto& left_positions = left_driver_->get_robot_output().joint.all.positions;
  std::vector<float> left_out(left_positions.begin(), left_positions.end());
  apply_joint_remap(left_out, joint_offsets_left_, false);

  const auto& right_positions = right_driver_->get_robot_output().joint.all.positions;
  std::vector<float> right_out(right_positions.begin(), right_positions.end());
  apply_joint_remap(right_out, joint_offsets_right_, false);

  std::vector<float> sample;
  sample.reserve(LEN);
  sample.assign(left_out.begin(), left_out.end());
  sample.insert(sample.end(), right_out.begin(), right_out.end());

  trossen_arm::RobotOutput::InputReport right_input = right_driver_->get_input_report();
  trossen_arm::RobotOutput::InputReport left_input = left_driver_ ->get_input_report();
  double base_vx = scale(left_input.joystick_x, MIN_JOYSTICK_, MAX_JOYSTICK_,
                                BASE_MIN_, BASE_MAX_, BASE_DEADZONE_);
  double base_vy = -scale(left_input.joystick_y, MIN_JOYSTICK_, MAX_JOYSTICK_,
                              BASE_MIN_, BASE_MAX_, BASE_DEADZONE_);
  double base_vr = -scale(right_input.joystick_x, MIN_JOYSTICK_, MAX_JOYSTICK_,
                                BASE_MIN_, BASE_MAX_, BASE_DEADZONE_);
  int right_up_btn = int(right_input.buttons & (1 << 0));
  int right_down_btn = int(right_input.buttons & (1 << 2));
  double base_vlift = double(right_up_btn - right_down_btn) * BASE_LIFT_MAX_;
  sample.push_back(static_cast<float>(base_vx));
  sample.push_back(static_cast<float>(base_vy));
  sample.push_back(static_cast<float>(base_vr));
  sample.push_back(static_cast<float>(base_vlift));
  return sample;
}

std::vector<float> BimanualGlideComponent::read_gripper_effort() {
  if (!left_driver_ || !right_driver_)  return {};
  return std::vector<float>({static_cast<float>(left_driver_->get_gripper_effort()),
    static_cast<float>(right_driver_->get_gripper_effort())});
}

void BimanualGlideComponent::apply_gripper_feedback(const std::vector<float>& follower_gripper_effort) {
  if (!left_driver_ || !right_driver_) return;

  const size_t EXPECTED_SIZE = 2;  // left and right
  if (follower_gripper_effort.size() != EXPECTED_SIZE) {
    throw std::invalid_argument("BimanualGlideComponent: Invalid command size");
  }

  float left_follower_effort = follower_gripper_effort[0];
  float right_follower_effort = follower_gripper_effort[1];

  // Cubic curve (from the bilateral reference): more resistance at higher grip
  // efforts, with an offset that keeps the leader gripper open when nothing is
  // grasped. leader = leader_max·norm^3 + offset.
  float left_norm = 0.0f, right_norm = 0.0f;
  if (gripper_feedback_follower_max_ != 0.0f) {
    left_norm = std::min(std::abs(left_follower_effort) / gripper_feedback_follower_max_, 1.0f);
    right_norm = std::min(std::abs(right_follower_effort) / gripper_feedback_follower_max_, 1.0f);
  }
  const double left_effort = gripper_feedback_leader_max_ * std::pow(left_norm, 3)
    + gripper_feedback_offset_;
  const double right_effort = gripper_feedback_leader_max_ * std::pow(right_norm, 3)
    + gripper_feedback_offset_;

  left_driver_->set_gripper_external_effort(left_effort, write_moving_time_s_, false);
  right_driver_->set_gripper_external_effort(right_effort, write_moving_time_s_, false);

}

std::vector<float> BimanualGlideComponent::read_cartesian() {
  if (!left_driver_ || !right_driver_) return {};
  const int LEN = 18; // (4 cart + 3 rotation vector + 1 gripper) * 2 + 4 base
  const auto& left_out = left_driver_->get_robot_output();
  const auto& right_out = right_driver_->get_robot_output();

  std::vector<float> sample;
  sample.reserve(LEN);
  sample.assign(left_out.cartesian.positions.begin(), left_out.cartesian.positions.end());
  sample.push_back(static_cast<float>(left_out.joint.gripper.position));
  sample.insert(sample.end(), right_out.cartesian.positions.begin(),
    right_out.cartesian.positions.end());
  sample.push_back(static_cast<float>(right_out.joint.gripper.position));
  trossen_arm::RobotOutput::InputReport right_input = right_driver_->get_input_report();
  trossen_arm::RobotOutput::InputReport left_input = left_driver_ ->get_input_report();
  double base_vx = scale(left_input.joystick_x, MIN_JOYSTICK_, MAX_JOYSTICK_,
                                BASE_MIN_, BASE_MAX_, BASE_DEADZONE_);
  double base_vy = -scale(left_input.joystick_y, MIN_JOYSTICK_, MAX_JOYSTICK_,
                              BASE_MIN_, BASE_MAX_, BASE_DEADZONE_);
  double base_vr = -scale(right_input.joystick_x, MIN_JOYSTICK_, MAX_JOYSTICK_,
                                BASE_MIN_, BASE_MAX_, BASE_DEADZONE_);
  int right_up_btn = int(right_input.buttons & (1 << 0));
  int right_down_btn = int(right_input.buttons & (1 << 2));
  double base_vlift = double(right_up_btn - right_down_btn) * BASE_LIFT_MAX_;
  sample.push_back(static_cast<float>(base_vx));
  sample.push_back(static_cast<float>(base_vy));
  sample.push_back(static_cast<float>(base_vr));
  sample.push_back(static_cast<float>(base_vlift));
  return sample;
}


// ── Shared lifecycle ─────────────────────────────────────────────────────

void BimanualGlideComponent::on_pre_episode() {
  // Nothing needed for glide
}
void BimanualGlideComponent::prepare_for_teleop(){
  // Configure mode
  right_driver_->set_gripper_mode(trossen_arm::Mode::external_effort);
  left_driver_->set_gripper_mode(trossen_arm::Mode::external_effort);
  right_driver_->set_all_modes(trossen_arm::Mode::external_effort);
  left_driver_->set_all_modes(trossen_arm::Mode::external_effort);
  
  left_driver_->set_gripper_external_effort(gripper_feedback_offset_, write_moving_time_s_, false);
  right_driver_->set_gripper_external_effort(gripper_feedback_offset_, write_moving_time_s_, false);
}
void BimanualGlideComponent::end_teleop(){}
void BimanualGlideComponent::stage(){}


double BimanualGlideComponent::scale(double val, double val_min, double val_max, double scaled_min,
  double scaled_max, double scaled_deadzone) {
    double scaled_val = (val - val_min) / (val_max - val_min) * (scaled_max - scaled_min)
      + scaled_min;

    if (scaled_deadzone && std::abs(scaled_val) < scaled_deadzone) return 0;

    return scaled_val;
  }


REGISTER_HARDWARE(BimanualGlideComponent,"bimanual_glide")

}  // namespace trossen::hw::bimanual_glide


