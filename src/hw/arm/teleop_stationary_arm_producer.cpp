/**
 * @file teleop_stationary_arm_producer.cpp
 * @brief Implementation of TeleopStationaryArmProducer for bimanual teleoperation
 */

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

#include "trossen_sdk/hw/arm/teleop_stationary_arm_producer.hpp"
#include "trossen_sdk/hw/arm/teleop_arm_component.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"

namespace trossen::hw::arm {

// Registry-compatible constructor with custom factory registration
TeleopStationaryArmProducer::TeleopStationaryArmProducer(
  std::shared_ptr<hw::HardwareComponent> left_hardware,
  std::shared_ptr<hw::HardwareComponent> right_hardware,
  const nlohmann::json& config)
{
  // Validate left hardware
  if (!left_hardware) {
    throw std::invalid_argument(
      "TeleopStationaryArmProducer: left hardware component cannot be null");
  }
  auto left_teleop = std::dynamic_pointer_cast<TeleopArmComponent>(left_hardware);
  if (!left_teleop) {
    throw std::invalid_argument(
      "TeleopStationaryArmProducer: left hardware must be TeleopArmComponent, got: " +
      left_hardware->get_type());
  }

  // Validate right hardware
  if (!right_hardware) {
    throw std::invalid_argument(
      "TeleopStationaryArmProducer: right hardware component cannot be null");
  }
  auto right_teleop = std::dynamic_pointer_cast<TeleopArmComponent>(right_hardware);
  if (!right_teleop) {
    throw std::invalid_argument(
      "TeleopStationaryArmProducer: right hardware must be TeleopArmComponent, got: " +
      right_hardware->get_type());
  }

  // Extract drivers
  auto left_drivers = left_teleop->get_hardware();
  auto right_drivers = right_teleop->get_hardware();

  left_leader_driver_ = left_drivers.leader;
  left_follower_driver_ = left_drivers.follower;
  right_leader_driver_ = right_drivers.leader;
  right_follower_driver_ = right_drivers.follower;

  if (!left_leader_driver_ || !left_follower_driver_ ||
      !right_leader_driver_ || !right_follower_driver_) {
    throw std::invalid_argument(
      "TeleopStationaryArmProducer: all drivers must be non-null");
  }

  cfg_.stream_id = config.value("stream_id", "teleop_stationary");
  cfg_.use_device_time = config.value("use_device_time", false);

  // Allocate buffers for both arms (concatenated)
  size_t left_joints = left_leader_driver_->get_num_joints();
  size_t right_joints = right_leader_driver_->get_num_joints();
  act_d_.resize(left_joints + right_joints);
  obs_d_.resize(left_joints + right_joints);

  left_leader_output_ = left_leader_driver_->get_robot_output();
  left_follower_output_ = left_follower_driver_->get_robot_output();
  right_leader_output_ = right_leader_driver_->get_robot_output();
  right_follower_output_ = right_follower_driver_->get_robot_output();

  metadata_.type = "teleop_stationary";
  metadata_.id = cfg_.stream_id;
  metadata_.name = "Teleop STATIONARY Arm Producer";
  metadata_.description =
      "Bimanual teleoperation for STATIONARY configuration (2 leaders + 2 followers)";
  metadata_.robot_name = "Trossen STATIONARY AI";
  metadata_.action_feature_names = {
    "left_joint1", "left_joint2", "left_joint3", "left_joint4",
    "left_joint5", "left_joint6", "left_gripper",
    "right_joint1", "right_joint2", "right_joint3", "right_joint4",
    "right_joint5", "right_joint6", "right_gripper"
  };
  metadata_.observation_feature_names = {
    "left_joint1", "left_joint2", "left_joint3", "left_joint4",
    "left_joint5", "left_joint6", "left_gripper",
    "right_joint1", "right_joint2", "right_joint3", "right_joint4",
    "right_joint5", "right_joint6", "right_gripper"
  };
  metadata_.action_dtype = "float32";
  metadata_.observation_dtype = "float32";
}

// Direct driver constructor
TeleopStationaryArmProducer::TeleopStationaryArmProducer(
  std::shared_ptr<trossen_arm::TrossenArmDriver> left_leader_driver,
  std::shared_ptr<trossen_arm::TrossenArmDriver> left_follower_driver,
  std::shared_ptr<trossen_arm::TrossenArmDriver> right_leader_driver,
  std::shared_ptr<trossen_arm::TrossenArmDriver> right_follower_driver,
  Config cfg)
  : left_leader_driver_(std::move(left_leader_driver)),
    left_follower_driver_(std::move(left_follower_driver)),
    right_leader_driver_(std::move(right_leader_driver)),
    right_follower_driver_(std::move(right_follower_driver)),
    cfg_(std::move(cfg))
{
  if (left_leader_driver_ && left_follower_driver_ &&
      right_leader_driver_ && right_follower_driver_) {
    size_t left_joints = left_leader_driver_->get_num_joints();
    size_t right_joints = right_leader_driver_->get_num_joints();
    act_d_.resize(left_joints + right_joints);
    obs_d_.resize(left_joints + right_joints);

    left_leader_output_ = left_leader_driver_->get_robot_output();
    left_follower_output_ = left_follower_driver_->get_robot_output();
    right_leader_output_ = right_leader_driver_->get_robot_output();
    right_follower_output_ = right_follower_driver_->get_robot_output();
  }

  metadata_.type = "teleop_stationary";
  metadata_.id = cfg_.stream_id;
  metadata_.name = "Teleop STATIONARY Arm Producer";
  metadata_.description =
      "Bimanual teleoperation for STATIONARY configuration (2 leaders + 2 followers)";
  metadata_.robot_name = "Trossen STATIONARY AI";
  metadata_.action_feature_names = {
    "left_joint1", "left_joint2", "left_joint3", "left_joint4",
    "left_joint5", "left_joint6", "left_gripper",
    "right_joint1", "right_joint2", "right_joint3", "right_joint4",
    "right_joint5", "right_joint6", "right_gripper"
  };
  metadata_.observation_feature_names = {
    "left_joint1", "left_joint2", "left_joint3", "left_joint4",
    "left_joint5", "left_joint6", "left_gripper",
    "right_joint1", "right_joint2", "right_joint3", "right_joint4",
    "right_joint5", "right_joint6", "right_gripper"
  };
  metadata_.action_dtype = "float32";
  metadata_.observation_dtype = "float32";
}

void TeleopStationaryArmProducer::poll(
  const std::function<void(std::shared_ptr<data::RecordBase>)>& emit)
{
  if (!left_leader_driver_ || !left_follower_driver_ ||
      !right_leader_driver_ || !right_follower_driver_) {
    return;
  }

  // Read all outputs
  left_leader_output_ = left_leader_driver_->get_robot_output();
  left_follower_output_ = left_follower_driver_->get_robot_output();
  right_leader_output_ = right_leader_driver_->get_robot_output();
  right_follower_output_ = right_follower_driver_->get_robot_output();

  uint64_t device_ts = left_leader_output_.header.timestamp;

  // Concatenate left and right positions
  auto left_leader_pos = left_leader_output_.joint.all.positions;
  auto right_leader_pos = right_leader_output_.joint.all.positions;
  auto left_follower_pos = left_follower_output_.joint.all.positions;
  auto right_follower_pos = right_follower_output_.joint.all.positions;

  act_d_.clear();
  act_d_.insert(act_d_.end(), left_leader_pos.begin(), left_leader_pos.end());
  act_d_.insert(act_d_.end(), right_leader_pos.begin(), right_leader_pos.end());

  obs_d_.clear();
  obs_d_.insert(obs_d_.end(), left_follower_pos.begin(), left_follower_pos.end());
  obs_d_.insert(obs_d_.end(), right_follower_pos.begin(), right_follower_pos.end());

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

// Custom factory registration for TeleopStationaryArmProducer
namespace {
struct TeleopStationaryArmProducerRegistrar {
  TeleopStationaryArmProducerRegistrar() {
    trossen::runtime::ProducerRegistry::register_producer(
      "teleop_stationary",
      [](std::shared_ptr<trossen::hw::HardwareComponent> hw,
         const nlohmann::json& cfg) -> std::shared_ptr<trossen::hw::PolledProducer> {
        // This producer requires two hardware components (left and right pairs)
        if (!cfg.contains("left_id") || !cfg.contains("right_id")) {
          throw std::invalid_argument(
            "TeleopStationaryArmProducer requires 'left_id' and 'right_id' in config");
        }

        std::string left_id = cfg["left_id"];
        std::string right_id = cfg["right_id"];

        auto left_hw = trossen::hw::ActiveHardwareRegistry::get(left_id);
        auto right_hw = trossen::hw::ActiveHardwareRegistry::get(right_id);

        if (!left_hw) {
          throw std::invalid_argument(
            "TeleopStationaryArmProducer: left hardware '" + left_id +
            "' not found in registry");
        }
        if (!right_hw) {
          throw std::invalid_argument(
            "TeleopStationaryArmProducer: right hardware '" + right_id +
            "' not found in registry");
        }

        return std::make_shared<TeleopStationaryArmProducer>(left_hw, right_hw, cfg);
      });
  }
};
static TeleopStationaryArmProducerRegistrar teleop_stationary_arm_producer_registrar;
}  // anonymous namespace

}  // namespace trossen::hw::arm
