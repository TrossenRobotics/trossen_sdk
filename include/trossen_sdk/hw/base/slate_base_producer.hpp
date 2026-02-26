/**
 * @file slate_base_producer.hpp
 * @brief Producer that emits velocity states from a SLATE mobile base
 */

#ifndef TROSSEN_SDK__HW__BASE__SLATE_BASE_PRODUCER_HPP_
#define TROSSEN_SDK__HW__BASE__SLATE_BASE_PRODUCER_HPP_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "trossen_slate/base_driver.hpp"
#include "trossen_slate/trossen_slate.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

namespace trossen::hw::base {

/**
 * @brief Producer that emits velocity states from SLATE mobile base
 *
 * Stores linear velocity (x, y) and angular velocity (z) in the JointStateRecord
 * velocity vector as [vel_x, vel_y, vel_z].
 */
class SlateBaseProducer : public ::trossen::hw::PolledProducer {
public:
  /**
   * @brief Configuration parameters for SlateBaseProducer
   */
  struct Config {
    /// @brief Logical stream identifier (e.g. "base_velocity")
    std::string stream_id{"base"};

    /// @brief Prefer device timestamp if available
    bool use_device_time{false};
  };

  /**
   * @brief Metadata specific to SlateBaseProducer
   */
  struct SlateBaseProducerMetadata : public PolledProducer::ProducerMetadata {
    /// @brief Base model type
    std::string base_model;

    /**
     * @brief Get producer info as JSON
     *
     * @return JSON object containing producer information
     */
    nlohmann::ordered_json get_info() const override {
      std::cout << "SlateBaseProducerMetadata: " << name << " (" << id << ") - " << description
                << ", Model: " << base_model << "\n";
      return nlohmann::ordered_json{};
    }
  };

  /**
   * @brief Construct a SlateBaseProducer from hardware component
   *
   * @param hardware Hardware component (must be SlateBaseComponent)
   * @param config JSON configuration with fields: stream_id, use_device_time
   * @throws std::invalid_argument if hardware is null or wrong type
   */
  SlateBaseProducer(
    std::shared_ptr<hw::HardwareComponent> hardware,
    const nlohmann::json& config);

  /**
   * @brief Destructor
   */
  ~SlateBaseProducer() override = default;

  /**
   * @brief Poll the driver for the latest velocity states and emit a JointStateRecord
   *
   * Stores velocity data in the velocity vector as [vel_x, vel_y, vel_z].
   * Position and effort vectors remain empty.
   *
   * @param emit Callback to invoke for each produced record
   */
  void poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;

  /**
   * @brief Get producer metadata
   *
   * @return const reference to ProducerMetadata
   */
  std::shared_ptr<ProducerMetadata> metadata() const override {
    return std::make_shared<SlateBaseProducerMetadata>(metadata_);
  }

private:
  /// @brief Shared pointer to the TrossenSlate driver instance
  std::shared_ptr<trossen_slate::TrossenSlate> driver_;

  /// @brief Configuration
  Config cfg_;

  /// @brief Metadata
  SlateBaseProducerMetadata metadata_;

  /// @brief Cached chassis data
  base_driver::ChassisData chassis_data_;
};

}  // namespace trossen::hw::base

#endif  // TROSSEN_SDK__HW__BASE__SLATE_BASE_PRODUCER_HPP_
