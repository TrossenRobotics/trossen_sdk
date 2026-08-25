/**
 * @file trossen_base_producer.cpp
 * @brief Implementation of TrossenBaseProducer.
 */

#include "trossen_sdk/hw/base/trossen_base_producer.hpp"

#include <memory>
#include <stdexcept>
#include <vector>

#include "trossen_sdk/hw/teleop/teleop_capable.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"

namespace trossen::hw::base {

namespace ba = teleop::base_axis;

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
    "Produces odometry and commanded velocity from the Rivet swerve base";
  metadata_.base_model = "Rivet";
}

void TrossenBaseProducer::poll(
  const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) {
  if (!driver_ || !component_) return;

  // Two threads reach this driver: this one, and the component's servicing
  // thread ticking update_base() at 15Hz. That is safe only because of which
  // fields each touches, so check before adding a read here.
  //
  // TrossenBase caches into one unsynchronised `base_data_` struct. get_pose()
  // writes its pose members; update_base() -> refresh_battery_status() writes
  // the battery members. Disjoint members of a struct are distinct memory
  // locations, so the two do not race, and the reads underneath both take the
  // driver's own status_mutex_ against its receive thread.
  //
  // The battery accessors are the trap: get_percent(), get_temp() and
  // get_charging_state() read exactly the members the servicing thread writes,
  // with nothing between them. Calling one here would be a genuine data race on
  // a non-atomic float. Battery telemetry is deliberately not recorded — see
  // Odometry2DRecord — which is what keeps this poll to disjoint state.
  const auto pose = driver_->get_pose();
  const auto command = component_->last_command();

  data::Timestamp ts;
  ts.monotonic = data::now_mono();
  ts.realtime = data::now_real();

  auto rec = std::make_shared<data::Odometry2DRecord>();
  rec->ts = ts;
  rec->seq = seq_++;
  rec->id = cfg_.stream_id;

  rec->pose.x     = pose[0];
  rec->pose.y     = pose[1];
  rec->pose.theta = pose[2];

  // Commanded, not measured — the base reports no velocity feedback. Read
  // through base_axis::get() so a future change to the echo's length cannot
  // walk off the end here.
  rec->twist.linear_x  = ba::get(command, ba::kLinear);
  rec->twist.linear_y  = ba::get(command, ba::kLateral);
  rec->twist.angular_z = ba::get(command, ba::kAngular);
  rec->lift_velocity   = ba::get(command, ba::kLift);

  emit(rec);
  stats_.produced++;
}

REGISTER_PRODUCER(TrossenBaseProducer, "trossen_base");

}  // namespace trossen::hw::base
