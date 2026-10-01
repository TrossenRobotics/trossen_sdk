/**
 * @file trossen_base_producer.hpp
 * @brief Producer that records pose and commanded velocity from the Rivet base.
 */

#ifndef TROSSEN_SDK__HW__BASE__TROSSEN_BASE_PRODUCER_HPP_
#define TROSSEN_SDK__HW__BASE__TROSSEN_BASE_PRODUCER_HPP_

#include <functional>
#include <memory>
#include <string>

#include "trossen_base/trossen_base.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/base/trossen_base_component.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

namespace trossen::hw::base {

/**
 * @brief Emits an Odometry2DRecord per poll for the Rivet swerve base.
 *
 * Pose and twist both come from the base's wheel odometry, so the twist is the
 * measured velocity, as on the SLATE. The lift rides in
 * `Odometry2DRecord::lift_velocity`, outside the 2D twist since vertical motion is
 * not planar, and is the lift's measured velocity in m/s.
 */
class TrossenBaseProducer : public ::trossen::hw::PolledProducer {
public:
  /// @brief Configuration parameters for TrossenBaseProducer
  struct Config {
    /// @brief Logical stream identifier.
    std::string stream_id{"base"};

    /// @brief Prefer a device timestamp when one is available.
    bool use_device_time{false};
  };

  /// @brief Metadata specific to TrossenBaseProducer
  struct TrossenBaseProducerMetadata : public PolledProducer::ProducerMetadata {
    /// @brief Base model type.
    std::string base_model;

    nlohmann::ordered_json get_info() const override {
      return nlohmann::ordered_json{};
    }

    /// Names the velocity pair a converter writes first in the base block. The
    /// lateral velocity and pose are added by the converter under its own names.
    nlohmann::ordered_json get_stream_info() const override {
      nlohmann::ordered_json info;
      info["has_mobile_base"] = true;
      info["base_velocity_names"] = nlohmann::json::array({"linear_vel", "angular_vel"});
      return info;
    }
  };

  /**
   * @brief Construct from a configured TrossenBaseComponent.
   *
   * @throws std::invalid_argument if @p hardware is null, is not a
   *         TrossenBaseComponent, or has no driver.
   */
  TrossenBaseProducer(
    std::shared_ptr<hw::HardwareComponent> hardware,
    const nlohmann::json& config);

  ~TrossenBaseProducer() override = default;

  /**
   * @brief Emit one Odometry2DRecord: measured pose, twist and lift velocity.
   *
   * Runs on the scheduler thread while the component's servicing thread ticks
   * the same driver, so what it may touch is constrained — see the note in the
   * implementation before adding a field here.
   */
  void poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;

  std::shared_ptr<ProducerMetadata> metadata() const override {
    return std::make_shared<TrossenBaseProducerMetadata>(metadata_);
  }

private:
  /// Held so the component, which owns the driver's servicing thread, outlives
  /// this producer.
  std::shared_ptr<TrossenBaseComponent> component_;

  std::shared_ptr<trossen_base::TrossenBase> driver_;

  Config cfg_;

  TrossenBaseProducerMetadata metadata_;
};

}  // namespace trossen::hw::base

#endif  // TROSSEN_SDK__HW__BASE__TROSSEN_BASE_PRODUCER_HPP_
