#include "trossen_sdk/hw/arm/arm_producer.hpp"

#include <algorithm>

namespace trossen::hw::arm {

TrossenArmProducer::TrossenArmProducer(
  std::shared_ptr<trossen_arm::TrossenArmDriver> driver,
  Config cfg)
  : driver_(std::move(driver)), cfg_(std::move(cfg))
{
  if (driver_) {
    // Preallocate buffers based on number of joints
    pos_d_.resize(driver_->get_num_joints());
    vel_d_.resize(driver_->get_num_joints());
    eff_d_.resize(driver_->get_num_joints());
    // Initial read to populate robot_output state
    robot_output_ = driver_->get_robot_output();
  }
}

void TrossenArmProducer::poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) {
  if (!driver_) {
    return;
  }

  // Read robot output, save timestamp and joint states
  robot_output_ = driver_->get_robot_output();
  uint64_t device_ts = robot_output_.header.timestamp;
  // TODO [shantanuparab-tr]: Get actions from the leader arm
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

} // namespace trossen::hw::arm
