#include <algorithm>
#include <iostream>
#include <memory>
#include <utility>

#include "trossen_sdk/hw/arm/so101_teleop_arm_producer.hpp"

namespace trossen::hw::arm {

TeleopSO101ArmProducer::TeleopSO101ArmProducer(
  std::shared_ptr<SO101Leader> leader,
  std::shared_ptr<SO101Follower> follower,
  Config cfg)
  : leader_(std::move(leader)),
    follower_(std::move(follower)),
    cfg_(std::move(cfg))
{
  if (leader_ && follower_) {
    // Get joint names from the devices
    auto leader_joints = leader_->getJointNames();
    auto follower_joints = follower_->getJointNames();

    // Preallocate buffers based on number of joints
    act_d_.resize(leader_joints.size());
    obs_d_.resize(follower_joints.size());

    // Store joint names for metadata
    metadata_.action_feature_names = leader_joints;
    metadata_.observation_feature_names = follower_joints;
  }

  // Populate metadata
  metadata_.type = "teleop_so101_arm";
  metadata_.id = cfg_.stream_id;
  metadata_.name = "Teleop SO-101 Arm Producer";
  metadata_.description =
    "Produces teleoperation joint states from leader and follower "
    "SO-101 Arms";

  metadata_.robot_name = "SO-101";
  metadata_.action_dtype = "float32";
  metadata_.observation_dtype = "float32";
}

void TeleopSO101ArmProducer::poll(
  const std::function<void(std::shared_ptr<data::RecordBase>)>& emit)
{
  if (!leader_ || !follower_) {
    return;
  }

  // Read joint positions from leader (actions) and follower (observations)
  leader_action_ = leader_->getJointPositions();
  follower_observation_ = follower_->getJointPositions();

  // Convert map values to vectors in consistent order
  act_d_.clear();
  act_d_.reserve(metadata_.action_feature_names.size());
  for (const auto& joint_name : metadata_.action_feature_names) {
    if (leader_action_.count(joint_name)) {
      act_d_.push_back(static_cast<double>(leader_action_[joint_name]));
    }
  }

  obs_d_.clear();
  obs_d_.reserve(metadata_.observation_feature_names.size());
  for (const auto& joint_name : metadata_.observation_feature_names) {
    if (follower_observation_.count(joint_name)) {
      obs_d_.push_back(static_cast<double>(follower_observation_[joint_name]));
    }
  }

  // Create record with appropriate timestamp
  data::Timestamp ts;
  ts.monotonic = data::now_mono();
  ts.realtime = data::now_real();

  // Create and populate TeleopJointStateRecord
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

}  // namespace trossen::hw::arm
