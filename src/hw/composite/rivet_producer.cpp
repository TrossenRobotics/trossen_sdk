/**
 * @file rivet_producer.cpp
 * @brief Implementation of RivetProducer.
 */

#include "trossen_sdk/hw/composite/rivet_producer.hpp"
#include "trossen_sdk/hw/composite/rivet_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"

namespace trossen::hw::rivet {

RivetProducer::RivetProducer(
  std::shared_ptr<hw::HardwareComponent> hardware,
  const nlohmann::json& config) {
  // Validate hardware component
  if (!hardware) {
    throw std::invalid_argument("RivetProducer: hardware component cannot be null");
  }

  // Dynamic cast to RivetComponent
  auto rivet_component = std::dynamic_pointer_cast<RivetComponent>(hardware);
  if (!rivet_component) {
    throw std::invalid_argument(
      "RivetProducer: hardware must be RivetComponent, got: " + hardware->get_type());
  }

  // Extract drivers
  left_driver_ = rivet_component->get_left_arm_hardware();
  right_driver_ = rivet_component->get_right_arm_hardware();
  if (!left_driver_ || !right_driver_) {
    throw std::invalid_argument("RivetProducer: RivetComponent has null arm driver(s)");
  }

  // Retained so poll() can read the base's last commanded velocities.
  rivet_component_ = std::move(rivet_component);

  // Parse JSON config into Config struct
  cfg_.stream_id = config.value("stream_id", "rivet");
  cfg_.use_device_time = config.value("use_device_time", false);

  // Populate metadata
  metadata_.type = "rivet";
  metadata_.id = cfg_.stream_id;
  metadata_.name = "Rivet Producer";
  metadata_.description = "Produces bimanual joint states and base velocities from Rivet";
  metadata_.arm_model = "PRO";
  metadata_.gripper_type = "STANDARD";

  // Initial read to size the per-arm joint name lists (gripper included)
  auto left_initial = left_driver_->get_robot_output();
  auto right_initial = right_driver_->get_robot_output();

  metadata_.left_joint_names.clear();
  for (size_t i = 0; i < left_initial.joint.all.positions.size(); ++i) {
    metadata_.left_joint_names.push_back("left_joint_" + std::to_string(i));
  }
  metadata_.right_joint_names.clear();
  for (size_t i = 0; i < right_initial.joint.all.positions.size(); ++i) {
    metadata_.right_joint_names.push_back("right_joint_" + std::to_string(i));
  }
}

void RivetProducer::poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) {
  if (!left_driver_ || !right_driver_) {
    return;
  }

  // Read robot output from both arms
  auto left_output = left_driver_->get_robot_output();
  auto right_output = right_driver_->get_robot_output();
  uint64_t device_ts = left_output.header.timestamp;

  // Create record with appropriate timestamp
  data::Timestamp ts;
  ts.monotonic = (cfg_.use_device_time && device_ts != 0) ?
    data::Timespec::from_ns(device_ts) : data::now_mono();
  ts.realtime = data::now_real();

  // Create and populate RivetRecord. Rivet's arms are followers (1:1 joint
  // mapping mirrored from teleop), so unlike TrossenArmProducer this records
  // the driver's raw joint states directly with no leader/follower remap.
  auto rec = std::make_shared<data::RivetRecord>();
  rec->ts = ts;
  rec->seq = seq_++;
  rec->id = cfg_.stream_id;

  // Convert double->float
  rec->left_positions.assign(
    left_output.joint.all.positions.begin(), left_output.joint.all.positions.end());
  rec->left_velocities.assign(
    left_output.joint.all.velocities.begin(), left_output.joint.all.velocities.end());
  rec->left_efforts.assign(
    left_output.joint.all.efforts.begin(), left_output.joint.all.efforts.end());
  rec->right_positions.assign(
    right_output.joint.all.positions.begin(), right_output.joint.all.positions.end());
  rec->right_velocities.assign(
    right_output.joint.all.velocities.begin(), right_output.joint.all.velocities.end());
  rec->right_efforts.assign(
    right_output.joint.all.efforts.begin(), right_output.joint.all.efforts.end());

  // Base exposes no measured velocity feedback, so record the last commanded
  // velocities tracked by RivetComponent (same values teleop wrote).
  auto base_vel = rivet_component_->get_base_velocities();
  rec->linear_x_velocity = static_cast<float>(base_vel.vx);
  rec->linear_y_velocity = static_cast<float>(base_vel.vy);
  rec->angular_z_velocity = static_cast<float>(base_vel.vr);
  rec->lift_velocity = static_cast<float>(base_vel.lift);

  auto base_driver = rivet_component_->get_base_hardware();
  rec->battery_percent = base_driver->get_percent();
  rec->battery_temp = base_driver->get_temp();
  rec->battery_charging_state = base_driver->get_charging_state();
  rec->battery_voltage = base_driver->get_voltage();
  rec->battery_has_critical_fault = base_driver->has_critical_fault();
  rec->is_e_stopped = base_driver->is_e_stopped();

  emit(rec);
  ++stats_.produced;
}

// Register producer with registry
REGISTER_PRODUCER(RivetProducer, "rivet");

}  // namespace trossen::hw::rivet
