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
 * Pose comes from the base's own odometry. Twist does not: the base reports no
 * measured velocity, so the recorded twist is the velocity most recently
 * *commanded* to it. For a learned policy that distinction matters — the twist
 * channel is an action echo, not an observation — which is why it is stated here
 * and in the stream metadata rather than left for a reader to infer.
 *
 * The lift and the battery telemetry ride in Odometry2DRecord's own fields for
 * them, outside the 2D twist, since neither is planar motion.
 */
class TrossenBaseProducer : public ::trossen::hw::PolledProducer {
public:
  struct Config {
    /// @brief Logical stream identifier.
    std::string stream_id{"base"};

    /// @brief Prefer a device timestamp when one is available.
    bool use_device_time{false};
  };

  struct TrossenBaseProducerMetadata : public PolledProducer::ProducerMetadata {
    /// @brief Base model type.
    std::string base_model;

    nlohmann::ordered_json get_info() const override {
      return nlohmann::ordered_json{};
    }

    /// Names the four axes this base actually drives, so a converter does not
    /// have to assume the 2-axis differential-drive layout.
    nlohmann::ordered_json get_stream_info() const override {
      nlohmann::ordered_json info;
      info["has_mobile_base"] = true;
      info["base_velocity_names"] = nlohmann::json::array(
        {"linear_vel", "angular_vel", "lift_vel", "lateral_vel"});
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

  /// Emit one Odometry2DRecord: measured pose, last-commanded twist.
  void poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;

  std::shared_ptr<ProducerMetadata> metadata() const override {
    return std::make_shared<TrossenBaseProducerMetadata>(metadata_);
  }

private:
  /// The component rather than the bare driver: the last commanded velocity is
  /// the component's state, since the base cannot report it back.
  std::shared_ptr<TrossenBaseComponent> component_;

  std::shared_ptr<trossen_base::TrossenBase> driver_;

  Config cfg_;

  TrossenBaseProducerMetadata metadata_;
};

}  // namespace trossen::hw::base

#endif  // TROSSEN_SDK__HW__BASE__TROSSEN_BASE_PRODUCER_HPP_
