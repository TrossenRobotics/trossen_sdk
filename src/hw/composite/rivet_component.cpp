/**
 * @file rivet_component.cpp
 * @brief Implementation of RivetComponent.
 */

#include "trossen_sdk/hw/composite/rivet_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"

namespace trossen::hw::rivet {
void RivetComponent::write_joint(const std::vector<float>& cmd)
{
  if (!left_driver_ || !right_driver_) return;
  int left_joint_count =  static_cast<int>(left_driver_->get_num_joints());
  int right_joint_count =  static_cast<int>(right_driver_->get_num_joints());
  size_t expected_cmd_size = static_cast<size_t>(left_joint_count+right_joint_count);

  if (cmd.size() != expected_cmd_size) {
    throw std::runtime_error(
      "TrossenArmComponent::write_joint: expected " + std::to_string(expected_cmd_size)
      + " joints, got " + std::to_string(cmd.size()));
  }


  std::vector<double> left_pos(cmd.begin(), cmd.end()-left_joint_count);
  std::vector<double> right_pos(cmd.begin()+left_joint_count, cmd.end());

  left_driver_->set_all_positions(left_pos, write_moving_time_s_, false);
  right_driver_->set_all_positions(right_pos, write_moving_time_s_, false);
}
}