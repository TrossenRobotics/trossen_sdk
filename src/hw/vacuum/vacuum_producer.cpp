/**
 * @file vacuum_producer.cpp
 * @brief Implementation of VacuumProducer.
 */

#include "trossen_sdk/hw/vacuum/vacuum_producer.hpp"

#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"

namespace trossen::hw::vacuum {

VacuumProducer::VacuumProducer(
  std::shared_ptr<hw::HardwareComponent> hardware,
  const nlohmann::json& config) {
  if (!hardware) {
    throw std::invalid_argument("VacuumProducer: hardware component cannot be null");
  }
  component_ = std::dynamic_pointer_cast<LcusVacuumComponent>(hardware);
  if (!component_) {
    throw std::invalid_argument(
      "VacuumProducer: hardware must be LcusVacuumComponent, got: " + hardware->get_type());
  }
  stream_id_ = config.value("stream_id", component_->get_identifier());

  metadata_.type = "vacuum";
  metadata_.id = stream_id_;
  metadata_.name = "Vacuum Producer";
  metadata_.description = "Commanded on/off state of a relay-switched vacuum gripper";
}

void VacuumProducer::poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) {
  const auto state = component_->commanded_on();
  const float value =
    state ? (*state ? 1.0f : 0.0f) : std::numeric_limits<float>::quiet_NaN();

  auto rec = std::make_shared<data::JointStateRecord>();
  rec->ts.monotonic = data::now_mono();
  rec->ts.realtime = data::now_real();
  rec->seq = seq_++;
  rec->id = stream_id_;
  rec->positions = {value};
  emit(rec);
  stats_.produced++;
}

REGISTER_PRODUCER(VacuumProducer, "lcus_vacuum");

}  // namespace trossen::hw::vacuum
