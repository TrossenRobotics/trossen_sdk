#include "trossen_sdk/hw/arm/teleop_arm_producer.hpp"

#include <algorithm>
#include <iostream>

namespace trossen::hw::arm {

TeleopTrossenArmProducer::TeleopTrossenArmProducer(
  std::shared_ptr<trossen_arm::TrossenArmDriver> leader_driver,
  std::shared_ptr<trossen_arm::TrossenArmDriver> follower_driver,
  Config cfg)
  : leader_driver_(std::move(leader_driver)),
    follower_driver_(std::move(follower_driver)),
    cfg_(std::move(cfg))
{
  if (leader_driver_ && follower_driver_) {
    // Preallocate buffers based on number of joints
    act_d_.resize(leader_driver_->get_num_joints());
    obs_d_.resize(follower_driver_->get_num_joints());
    // Initial read to populate robot_output state
    leader_robot_output_ = leader_driver_->get_robot_output();
    follower_robot_output_ = follower_driver_->get_robot_output();
  }

  // Populate metadata
  metadata_.type = "teleop_arm";
  metadata_.id = cfg_.stream_id;
  metadata_.name = "Teleop Trossen Arm Producer";
  metadata_.description = "Produces teleoperation joint states from leader and follower Trossen Arms via TrossenArmDriver";
  metadata_.robot_name = "Trossen AI Bimanual"; // TODO: Extract from driver/User Config
  metadata_.leader_arm_model = "WIDOWX_AI"; // TODO: Extract from driver/User Config
  metadata_.follower_arm_model = "WIDOWX_AI"; // TODO: Extract from driver/User Config
  metadata_.leader_firmware_version = "v1.9.0"; // TODO: Extract from driver / Remove
  metadata_.follower_firmware_version = "v1.9.0"; // TODO: Extract from driver / Remove
  metadata_.action_feature_names = {"joint1", "joint2", "joint3", "joint4", "joint5", "joint6", "gripper"};
  metadata_.observation_feature_names = {"joint1", "joint2", "joint3", "joint4", "joint5", "joint6", "gripper"};
  metadata_.gripper_type = "STANDARD"; // TODO: Extract from driver / User Config
  metadata_.action_dtype = "float32";
  metadata_.observation_dtype = "float32";

}

void TeleopTrossenArmProducer::poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) {
  if (!leader_driver_ || !follower_driver_) {
    return;
  }

  // Read robot output, save timestamp and joint states
  leader_robot_output_ = leader_driver_->get_robot_output();
  follower_robot_output_ = follower_driver_->get_robot_output();
  uint64_t device_ts = leader_robot_output_.header.timestamp;
  // TODO [shantanuparab-tr]: Get actions from the leader arm
  act_d_ = leader_robot_output_.joint.all.positions;
  obs_d_ = follower_robot_output_.joint.all.positions;

  // Create record with appropriate timestamp
  data::Timestamp ts;
  uint64_t mono_now = data::now_mono().to_ns();
  ts.monotonic = (cfg_.use_device_time && device_ts != 0) ?
    data::Timespec::from_ns(device_ts) : data::now_mono();
  ts.realtime = data::now_real();

  // Create and populate JointStateRecord
  auto rec = std::make_shared<data::TeleopJointStateRecord>();
  rec->ts = ts;
  rec->seq = seq_++;
  rec->id = cfg_.stream_id;
  // Convert double->float
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

} // namespace trossen::hw::arm
