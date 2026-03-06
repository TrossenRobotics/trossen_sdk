/**
 * @file arm_producer.cpp
 * @brief Implementation of TrossenArmProducer that emits joint states from a Trossen Arm via
 * TrossenArmDriver.
 */

#include <algorithm>
#include <memory>
#include <utility>
#include <stdexcept>

#include "trossen_sdk/hw/arm/trossen_arm_producer.hpp"
#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"

namespace trossen::hw::arm {

TrossenArmProducer::TrossenArmProducer(
  std::shared_ptr<hw::HardwareComponent> hardware,
  const nlohmann::json& config) {
  // Validate hardware component
  if (!hardware) {
    throw std::invalid_argument("TrossenArmProducer: hardware component cannot be null");
  }

  // Dynamic cast to TrossenArmComponent
  auto arm_component = std::dynamic_pointer_cast<TrossenArmComponent>(hardware);
  if (!arm_component) {
    throw std::invalid_argument(
      "TrossenArmProducer: hardware must be TrossenArmComponent, got: " + hardware->get_type());
  }

  // Extract driver
  driver_ = arm_component->get_hardware();
  if (!driver_) {
    throw std::invalid_argument("TrossenArmProducer: TrossenArmComponent has null driver");
  }

  // Parse JSON config into Config struct
  cfg_.stream_id = config.value("stream_id", "joint_states");
  cfg_.use_device_time = config.value("use_device_time", false);

  // Preallocate buffers based on number of joints
  pos_d_.resize(driver_->get_num_joints());
  vel_d_.resize(driver_->get_num_joints());
  eff_d_.resize(driver_->get_num_joints());

  // Initial read to populate robot_output state
  robot_output_ = driver_->get_robot_output();

  // Populate metadata
  metadata_.type = "arm";
  metadata_.id = cfg_.stream_id;
  metadata_.name = "Trossen Arm Producer";
  metadata_.description = "Produces joint states from a Trossen Arm via TrossenArmDriver";
  metadata_.arm_model = "WIDOWX_AI";
  size_t n = driver_->get_num_joints();
  metadata_.joint_names.clear();
  for (size_t i = 0; i < n; ++i) {
    metadata_.joint_names.push_back("joint_" + std::to_string(i));
  }
  metadata_.gripper_type = "STANDARD";
}

void TrossenArmProducer::poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) {
  if (!driver_) {
    return;
  }

  // Read robot output, save timestamp and joint states
  robot_output_ = driver_->get_robot_output();
  uint64_t device_ts = robot_output_.header.timestamp;
  // TODO(shantanuparab-tr): Get actions from the leader arm
  pos_d_ = robot_output_.joint.all.positions;
  vel_d_ = robot_output_.joint.all.velocities;
  eff_d_ = robot_output_.joint.all.efforts;

  // Create record with appropriate timestamp
  data::Timestamp ts;
  uint64_t mono_now = data::now_mono().to_ns();
  ts.monotonic = (cfg_.use_device_time && device_ts != 0) ?
    data::Timespec::from_ns(device_ts) : data::now_mono();
  ts.realtime = data::now_real();

  // Create and populate JointStateRecord
  auto rec = std::make_shared<data::JointStateRecord>();
  rec->ts = ts;
  rec->seq = seq_++;
  rec->id = cfg_.stream_id;
  // Convert double->float
  rec->positions.reserve(pos_d_.size());
  rec->velocities.reserve(vel_d_.size());
  rec->efforts.reserve(eff_d_.size());
  rec->positions.clear();
  rec->velocities.clear();
  rec->efforts.clear();
  for (double v : pos_d_) {
    rec->positions.push_back(static_cast<float>(v));
  }
  for (double v : vel_d_) {
    rec->velocities.push_back(static_cast<float>(v));
  }
  for (double v : eff_d_) {
    rec->efforts.push_back(static_cast<float>(v));
  }

  emit(rec);
  ++stats_.produced;
}

// Register producer with registry
REGISTER_PRODUCER(TrossenArmProducer, "trossen_arm");

}  // namespace trossen::hw::arm
