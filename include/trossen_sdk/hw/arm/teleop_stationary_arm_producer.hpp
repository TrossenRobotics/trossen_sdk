/**
 * @file teleop_stationary_arm_producer.hpp
 * @brief Producer for bimanual teleoperation (STATIONARY configuration)
 */

#ifndef TROSSEN_SDK__HW__ARM__TELEOP_STATIONARY_ARM_PRODUCER_HPP
#define TROSSEN_SDK__HW__ARM__TELEOP_STATIONARY_ARM_PRODUCER_HPP

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"
#include "nlohmann/json.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

namespace trossen::hw::arm {

/**
 * @brief Producer for bimanual teleoperation (STATIONARY configuration)
 *
 * Records joint states from two leader-follower pairs (left and right arms).
 */
class TeleopStationaryArmProducer : public ::trossen::hw::PolledProducer {
public:
  struct Config {
    std::string stream_id{"teleop_stationary"};
    bool use_device_time{true};
  };

  struct TeleopStationaryArmProducerMetadata : public PolledProducer::ProducerMetadata {
    std::string robot_name;
    std::vector<std::string> action_feature_names;
    std::vector<std::string> observation_feature_names;
    std::string action_dtype;
    std::string observation_dtype;

    nlohmann::ordered_json get_info() const override {
      nlohmann::ordered_json features;
      nlohmann::ordered_json action;
      action["dtype"] = action_dtype;
      action["shape"] = {static_cast<int>(action_feature_names.size())};
      action["names"] = action_feature_names;

      nlohmann::ordered_json observation_state;
      observation_state["dtype"] = observation_dtype;
      observation_state["shape"] = {static_cast<int>(observation_feature_names.size())};
      observation_state["names"] = observation_feature_names;
      features["action"] = action;
      features["observation.state"] = observation_state;
      return features;
    }
  };

  /**
   * @brief Construct from two TeleopArmComponents (registry pattern)
   *
   * @param left_hardware TeleopArmComponent for left arm pair
   * @param right_hardware TeleopArmComponent for right arm pair
   * @param config JSON configuration
   */
  TeleopStationaryArmProducer(
    std::shared_ptr<hw::HardwareComponent> left_hardware,
    std::shared_ptr<hw::HardwareComponent> right_hardware,
    const nlohmann::json& config);

  /**
   * @brief Construct directly from drivers
   */
  TeleopStationaryArmProducer(
    std::shared_ptr<trossen_arm::TrossenArmDriver> left_leader_driver,
    std::shared_ptr<trossen_arm::TrossenArmDriver> left_follower_driver,
    std::shared_ptr<trossen_arm::TrossenArmDriver> right_leader_driver,
    std::shared_ptr<trossen_arm::TrossenArmDriver> right_follower_driver,
    Config cfg);

  ~TeleopStationaryArmProducer() override = default;

  void poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;

  std::shared_ptr<ProducerMetadata> metadata() const override {
    return std::make_shared<TeleopStationaryArmProducerMetadata>(metadata_);
  }

private:
  std::shared_ptr<trossen_arm::TrossenArmDriver> left_leader_driver_;
  std::shared_ptr<trossen_arm::TrossenArmDriver> left_follower_driver_;
  std::shared_ptr<trossen_arm::TrossenArmDriver> right_leader_driver_;
  std::shared_ptr<trossen_arm::TrossenArmDriver> right_follower_driver_;
  Config cfg_;
  std::vector<double> act_d_, obs_d_;
  trossen_arm::RobotOutput left_leader_output_;
  trossen_arm::RobotOutput left_follower_output_;
  trossen_arm::RobotOutput right_leader_output_;
  trossen_arm::RobotOutput right_follower_output_;
  TeleopStationaryArmProducerMetadata metadata_;
};

}  // namespace trossen::hw::arm

#endif  // TROSSEN_SDK__HW__ARM__TELEOP_STATIONARY_ARM_PRODUCER_HPP
