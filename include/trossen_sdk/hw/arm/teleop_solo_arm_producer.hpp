/**
 * @file teleop_solo_arm_producer.hpp
 * @brief Producer for single leader-follower teleoperation (SOLO configuration)
 */

#ifndef TROSSEN_SDK__HW__ARM__TELEOP_SOLO_ARM_PRODUCER_HPP
#define TROSSEN_SDK__HW__ARM__TELEOP_SOLO_ARM_PRODUCER_HPP

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
 * @brief Producer for single leader-follower teleoperation (SOLO configuration)
 *
 * Records joint states from one leader and one follower arm.
 */
class TeleopSoloArmProducer : public ::trossen::hw::PolledProducer {
public:
  struct Config {
    std::string stream_id{"teleop_solo"};
    bool use_device_time{true};
  };

  struct TeleopSoloArmProducerMetadata : public PolledProducer::ProducerMetadata {
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
   * @brief Construct from TeleopArmComponent (registry pattern)
   */
  TeleopSoloArmProducer(
    std::shared_ptr<hw::HardwareComponent> hardware,
    const nlohmann::json& config);

  /**
   * @brief Construct directly from drivers
   */
  TeleopSoloArmProducer(
    std::shared_ptr<trossen_arm::TrossenArmDriver> leader_driver,
    std::shared_ptr<trossen_arm::TrossenArmDriver> follower_driver,
    Config cfg);

  ~TeleopSoloArmProducer() override = default;

  void poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;

  std::shared_ptr<ProducerMetadata> metadata() const override {
    return std::make_shared<TeleopSoloArmProducerMetadata>(metadata_);
  }

private:
  std::shared_ptr<trossen_arm::TrossenArmDriver> leader_driver_;
  std::shared_ptr<trossen_arm::TrossenArmDriver> follower_driver_;
  Config cfg_;
  std::vector<double> act_d_, obs_d_;
  trossen_arm::RobotOutput leader_robot_output_;
  trossen_arm::RobotOutput follower_robot_output_;
  TeleopSoloArmProducerMetadata metadata_;
};

}  // namespace trossen::hw::arm

#endif  // TROSSEN_SDK__HW__ARM__TELEOP_SOLO_ARM_PRODUCER_HPP
