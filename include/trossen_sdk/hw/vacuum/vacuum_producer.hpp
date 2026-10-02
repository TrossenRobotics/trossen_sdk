/**
 * @file vacuum_producer.hpp
 * @brief Producer that records the commanded state of a vacuum gripper.
 */

#ifndef TROSSEN_SDK__HW__VACUUM__VACUUM_PRODUCER_HPP_
#define TROSSEN_SDK__HW__VACUUM__VACUUM_PRODUCER_HPP_

#include <functional>
#include <memory>
#include <string>

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/hw/vacuum/lcus_vacuum_component.hpp"

namespace trossen::hw::vacuum {

/**
 * @brief Emits the vacuum's commanded state as a one-joint JointStateRecord.
 *
 * `positions[0]` is 1 while suction is on, 0 while it is off, and NaN while
 * the state is unknown (before the first press, or after a relay write
 * failed). It is what was commanded, not a pressure reading.
 *
 * Recorded on `<stream_id>/joints/state` like an arm. The LeRobot converters
 * set a joint stream aside when its width differs from the arms', so this
 * stream stays in the MCAP without changing `action` or `observation.state`.
 *
 * Expected JSON (a `producers` entry):
 * @code
 * { "type": "lcus_vacuum", "hardware_id": "vacuum_right",
 *   "stream_id": "vacuum_right", "poll_rate_hz": 30.0 }
 * @endcode
 */
class VacuumProducer : public ::trossen::hw::PolledProducer {
public:
  struct VacuumProducerMetadata : public PolledProducer::ProducerMetadata {
    nlohmann::ordered_json get_stream_info() const override {
      nlohmann::ordered_json info;
      info["streams"][id]["joint_names"] = nlohmann::json::array({"vacuum_on"});
      return info;
    }
  };

  /**
   * @throws std::invalid_argument if @p hardware is null or not an
   *         LcusVacuumComponent.
   */
  VacuumProducer(std::shared_ptr<hw::HardwareComponent> hardware, const nlohmann::json& config);

  ~VacuumProducer() override = default;

  void poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;

  std::shared_ptr<ProducerMetadata> metadata() const override {
    return std::make_shared<VacuumProducerMetadata>(metadata_);
  }

private:
  std::shared_ptr<LcusVacuumComponent> component_;
  std::string stream_id_;
  VacuumProducerMetadata metadata_;
};

}  // namespace trossen::hw::vacuum

#endif  // TROSSEN_SDK__HW__VACUUM__VACUUM_PRODUCER_HPP_
