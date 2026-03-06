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
 * @brief Producer that emits 2D pose and velocity from the SLATE mobile base.
 *
 * Reads odometry (odom_x, odom_y, odom_z) and body-frame velocity
 * (vel_x, vel_y, vel_z) from the SLATE driver and emits an Odometry2DRecord
 * per poll cycle.
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
      return nlohmann::ordered_json{};
    }

    /**
     * @brief Get per-stream dataset metadata for MCAP recording
     *
     * @return JSON indicating a mobile base is present with its velocity feature names
     */
    nlohmann::ordered_json get_stream_info() const override {
      nlohmann::ordered_json info;
      info["has_mobile_base"] = true;
      info["base_velocity_names"] = nlohmann::json::array({"linear_vel", "angular_vel"});
      return info;
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
   * @brief Poll the driver for the latest state and emit an Odometry2DRecord.
   *
   * Reads odom_x/y/z (pose) and vel_x/y/z (body-frame velocity) from the
   * SLATE driver and populates a Odometry2DRecord.
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
