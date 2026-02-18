/**
 * @file teleop_solo_arm_producer.cpp
 * @brief Implementation of TeleopSoloArmProducer for single leader-follower teleoperation
 */

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

#include "trossen_sdk/hw/arm/teleop_solo_arm_producer.hpp"
#include "trossen_sdk/hw/arm/teleop_arm_component.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"

namespace trossen::hw::arm {

// Registry-compatible constructor
TeleopSoloArmProducer::TeleopSoloArmProducer(
  std::shared_ptr<hw::HardwareComponent> hardware,
  const nlohmann::json& config)
{
  if (!hardware) {
    throw std::invalid_argument(
      "TeleopSoloArmProducer: hardware component cannot be null");
  }
  auto teleop_component = std::dynamic_pointer_cast<TeleopArmComponent>(hardware);
  if (!teleop_component) {
    throw std::invalid_argument(
      "TeleopSoloArmProducer: hardware must be TeleopArmComponent, got: " +
      hardware->get_type());
  }

  auto drivers = teleop_component->get_hardware();
  leader_driver_ = drivers.leader;
  follower_driver_ = drivers.follower;

  if (!leader_driver_) {
    throw std::invalid_argument(
      "TeleopSoloArmProducer: TeleopArmComponent has null leader driver");
  }
  if (!follower_driver_) {
    throw std::invalid_argument(
      "TeleopSoloArmProducer: TeleopArmComponent has null follower driver");
  }

  cfg_.stream_id = config.value("stream_id", "teleop_solo");
  cfg_.use_device_time = config.value("use_device_time", false);

  act_d_.resize(leader_driver_->get_num_joints());
  obs_d_.resize(follower_driver_->get_num_joints());

  leader_robot_output_ = leader_driver_->get_robot_output();
  follower_robot_output_ = follower_driver_->get_robot_output();

  metadata_.type = "teleop_solo";
  metadata_.id = cfg_.stream_id;
  metadata_.name = "Teleop SOLO Arm Producer";
  metadata_.description = "Single leader-follower teleoperation for SOLO configuration";
  metadata_.robot_name = "Trossen SOLO AI";
  metadata_.action_feature_names =
    {"joint1", "joint2", "joint3", "joint4", "joint5", "joint6", "gripper"};
  metadata_.observation_feature_names =
    {"joint1", "joint2", "joint3", "joint4", "joint5", "joint6", "gripper"};
  metadata_.action_dtype = "float32";
  metadata_.observation_dtype = "float32";
}

// Direct driver constructor
TeleopSoloArmProducer::TeleopSoloArmProducer(
  std::shared_ptr<trossen_arm::TrossenArmDriver> leader_driver,
  std::shared_ptr<trossen_arm::TrossenArmDriver> follower_driver,
  Config cfg)
  : leader_driver_(std::move(leader_driver)),
    follower_driver_(std::move(follower_driver)),
    cfg_(std::move(cfg))
{
  if (leader_driver_ && follower_driver_) {
    act_d_.resize(leader_driver_->get_num_joints());
    obs_d_.resize(follower_driver_->get_num_joints());
    leader_robot_output_ = leader_driver_->get_robot_output();
    follower_robot_output_ = follower_driver_->get_robot_output();
  }

  metadata_.type = "teleop_solo";
  metadata_.id = cfg_.stream_id;
  metadata_.name = "Teleop SOLO Arm Producer";
  metadata_.description = "Single leader-follower teleoperation for SOLO configuration";
  metadata_.robot_name = "Trossen SOLO AI";
  metadata_.action_feature_names =
    {"joint1", "joint2", "joint3", "joint4", "joint5", "joint6", "gripper"};
  metadata_.observation_feature_names =
    {"joint1", "joint2", "joint3", "joint4", "joint5", "joint6", "gripper"};
  metadata_.action_dtype = "float32";
  metadata_.observation_dtype = "float32";
}

void TeleopSoloArmProducer::poll(
  const std::function<void(std::shared_ptr<data::RecordBase>)>& emit)
{
  if (!leader_driver_ || !follower_driver_) {
    return;
  }

  leader_robot_output_ = leader_driver_->get_robot_output();
  follower_robot_output_ = follower_driver_->get_robot_output();
  uint64_t device_ts = leader_robot_output_.header.timestamp;
  act_d_ = leader_robot_output_.joint.all.positions;
  obs_d_ = follower_robot_output_.joint.all.positions;

  data::Timestamp ts;
  ts.monotonic = (cfg_.use_device_time && device_ts != 0) ?
    data::Timespec::from_ns(device_ts) : data::now_mono();
  ts.realtime = data::now_real();

  auto rec = std::make_shared<data::TeleopJointStateRecord>();
  rec->ts = ts;
  rec->seq = seq_++;
  rec->id = cfg_.stream_id;

  rec->actions.reserve(act_d_.size());
  rec->actions.clear();
  rec->observations.reserve(obs_d_.size());
  rec->observations.clear();
  for (double v : act_d_) {
    rec->actions.push_back(static_cast<float>(v));
  }
  for (double v : obs_d_) {
    rec->observations.push_back(static_cast<float>(v));
  }

  emit(rec);
  ++stats_.produced;
}

REGISTER_PRODUCER(TeleopSoloArmProducer, "teleop_solo")

}  // namespace trossen::hw::arm
