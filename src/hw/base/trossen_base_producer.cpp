/**
 * @file trossen_base_producer.cpp
 * @brief Implementation of TrossenBaseProducer.
 */

#include "trossen_sdk/hw/base/trossen_base_producer.hpp"

#include <memory>
#include <stdexcept>
#include <vector>

#include "trossen_sdk/runtime/producer_registry.hpp"

namespace trossen::hw::base {

TrossenBaseProducer::TrossenBaseProducer(
  std::shared_ptr<hw::HardwareComponent> hardware,
  const nlohmann::json& config) {
  if (!hardware) {
    throw std::invalid_argument("TrossenBaseProducer: hardware component cannot be null");
  }

  component_ = std::dynamic_pointer_cast<TrossenBaseComponent>(hardware);
  if (!component_) {
    throw std::invalid_argument(
      "TrossenBaseProducer: hardware must be TrossenBaseComponent, got: " +
      hardware->get_type());
  }

  driver_ = component_->get_driver();
  if (!driver_) {
    throw std::invalid_argument(
      "TrossenBaseProducer: TrossenBaseComponent has null driver; configure it first");
  }

  cfg_.stream_id = config.value("stream_id", "base");
  cfg_.use_device_time = config.value("use_device_time", false);

  metadata_.type = "base";
  metadata_.id = cfg_.stream_id;
  metadata_.name = "Trossen Base Producer";
  metadata_.description =
    "Produces wheel odometry and lift velocity from the Rivet swerve base";
  metadata_.base_model = "Rivet";
}

void TrossenBaseProducer::poll(
  const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) {
  if (!driver_ || !component_) return;

  // Two threads reach this driver: this one, and the component's servicing
  // thread sending heartbeat() at 15Hz. get_wheel_odometry() and get_lift_state()
  // each return a copy taken under the driver's own status mutex, so reading them
  // here is safe.
  const auto odometry = driver_->get_wheel_odometry();
  const auto lift = driver_->get_lift_state();

  data::Timestamp ts;
  ts.monotonic = data::now_mono();
  ts.realtime = data::now_real();

  auto rec = std::make_shared<data::Odometry2DRecord>();
  rec->ts = ts;
  rec->seq = seq_++;
  rec->id = cfg_.stream_id;

  // pose is [x, y, theta] (m, rad) and twist [vx, vy, vyaw] (m/s, rad/s).
  rec->pose.x     = odometry.pose[0];
  rec->pose.y     = odometry.pose[1];
  rec->pose.theta = odometry.pose[2];

  rec->twist.linear_x  = odometry.twist[0];
  rec->twist.linear_y  = odometry.twist[1];
  rec->twist.angular_z = odometry.twist[2];
  rec->lift_velocity   = lift.velocity;

  emit(rec);
  stats_.produced++;
}

REGISTER_PRODUCER(TrossenBaseProducer, "trossen_base");

}  // namespace trossen::hw::base
