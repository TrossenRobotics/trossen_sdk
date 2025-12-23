#include <algorithm>
#include <iostream>
#include <memory>
#include <utility>

#include "trossen_sdk/hw/arm/so101_teleop_arm_producer.hpp"

namespace trossen::hw::arm {

TeleopSO101ArmProducer::TeleopSO101ArmProducer(
  std::shared_ptr<SO101ArmDriver> leader,
  std::shared_ptr<SO101ArmDriver> follower,
  Config cfg)
  : leader_(std::move(leader)),
    follower_(std::move(follower)),
    cfg_(std::move(cfg))
{
  if (leader_ && follower_) {
    // Get joint names from the devices
    auto leader_joints = leader_->get_joint_names();
    auto follower_joints = follower_->get_joint_names();

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
  // These now return vectors directly in the correct order
  auto leader_positions = leader_->get_joint_positions();
  auto follower_positions = follower_->get_joint_positions();

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
  rec->actions.reserve(leader_positions.size());
  rec->observations.reserve(follower_positions.size());

  for (double v : leader_positions) {
    rec->actions.push_back(static_cast<float>(v));
  }
  for (double v : follower_positions) {
    rec->observations.push_back(static_cast<float>(v));
  }

  emit(rec);
  ++stats_.produced;
}

}  // namespace trossen::hw::arm
