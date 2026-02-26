/**
 * @file slate_base_producer.cpp
 * @brief Implementation of SlateBaseProducer that emits velocity states from SLATE mobile base
 */

#include <memory>
#include <stdexcept>
#include <utility>

#include "trossen_sdk/hw/base/slate_base_component.hpp"
#include "trossen_sdk/hw/base/slate_base_producer.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"

namespace trossen::hw::base {

SlateBaseProducer::SlateBaseProducer(
  std::shared_ptr<hw::HardwareComponent> hardware,
  const nlohmann::json& config) {
  // Validate hardware component
  if (!hardware) {
    throw std::invalid_argument("SlateBaseProducer: hardware component cannot be null");
  }

  // Dynamic cast to SlateBaseComponent
  auto base_component = std::dynamic_pointer_cast<SlateBaseComponent>(hardware);
  if (!base_component) {
    throw std::invalid_argument(
      "SlateBaseProducer: hardware must be SlateBaseComponent, got: " + hardware->get_type());
  }

  // Extract driver
  driver_ = base_component->get_driver();
  if (!driver_) {
    throw std::invalid_argument("SlateBaseProducer: SlateBaseComponent has null driver");
  }

  // Parse JSON config into Config struct
  cfg_.stream_id = config.value("stream_id", "base");
  cfg_.use_device_time = config.value("use_device_time", false);

  // Initialize chassis data
  chassis_data_ = {};

  // Populate metadata
  metadata_.type = "base";
  metadata_.id = cfg_.stream_id;
  metadata_.name = "SLATE Base Producer";
  metadata_.description = "Produces velocity states from SLATE mobile base";
  metadata_.base_model = "SLATE";
}

void SlateBaseProducer::poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) {
  if (!driver_) {
    return;
  }

  // Read chassis data from driver
  driver_->read(chassis_data_);

  // Create timestamp
  data::Timestamp ts;
  ts.monotonic = data::now_mono();
  ts.realtime = data::now_real();

  // Create and populate JointStateRecord with velocity data
  auto rec = std::make_shared<data::JointStateRecord>();
  rec->ts = ts;
  rec->seq = seq_++;
  rec->id = cfg_.stream_id;

  // Store velocities: [linear_x, linear_y, angular_z]
  rec->velocities.clear();
  rec->velocities.reserve(3);
  rec->velocities.push_back(chassis_data_.vel_x);
  rec->velocities.push_back(chassis_data_.vel_y);
  rec->velocities.push_back(chassis_data_.vel_z);

  // Leave positions and efforts empty
  rec->positions.clear();
  rec->efforts.clear();

  // Emit the record
  emit(rec);

  // Update statistics
  stats_.produced++;
}

// Register producer with registry
REGISTER_PRODUCER(SlateBaseProducer, "slate_base");

}  // namespace trossen::hw::base
