#ifndef TROSSEN_SDK__HW__ARM__TELEOP_ARM_PRODUCER_HPP
#define TROSSEN_SDK__HW__ARM__TELEOP_ARM_PRODUCER_HPP

#include <vector>
#include <memory>
#include <string>
#include <functional>

#include "libtrossen_arm/trossen_arm.hpp"

#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"

namespace trossen::hw::arm {

class TeleopTrossenArmProducer : public ::trossen::hw::PolledProducer {
public:
  struct Config {
    /// @brief Logical stream identifier (e.g. "arm_left")
    std::string stream_id{"teleop_arm"};

    /// @brief Prefer device timestamp if available
    bool use_device_time{true};
  };

  struct TeleopTrossenArmProducerMetadata : public PolledProducer::ProducerMetadata {
    /// @brief Robot name
    std::string robot_name;

    /// @brief Action feature names
    std::vector<std::string> action_feature_names;
    
    /// @brief Observation feature names
    std::vector<std::string> observation_feature_names;

    /// @brief Action feature data type
    std::string action_dtype;

    /// @brief Observation feature data type
    std::string observation_dtype;
  };

  /**
   * @brief Construct a TeleopTrossenArmProducer
   *
   * @param driver Shared pointer to an initialized TrossenArmDriver instance
   * @param cfg Configuration parameters
   */
  TeleopTrossenArmProducer(std::shared_ptr<trossen_arm::TrossenArmDriver> leader_driver,
                           std::shared_ptr<trossen_arm::TrossenArmDriver> follower_driver,
                           Config cfg);

  /**
   * @brief Destructor
   */
  ~TeleopTrossenArmProducer() override = default;

  /**
   * @brief Poll the driver for the latest joint states and emit a JointStateRecord
   *
   * @param emit Callback to invoke for each produced record
   */
  void poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;

  /// @brief Get producer metadata
  std::shared_ptr<ProducerMetadata> metadata() const override {
    return std::make_shared<TeleopTrossenArmProducerMetadata>(metadata_);
  }

private:
  /// @brief Shared pointer to the TrossenArmDriver instance leader arm
  std::shared_ptr<trossen_arm::TrossenArmDriver> leader_driver_;

  /// @brief Shared pointer to the TrossenArmDriver instance follower arm
  std::shared_ptr<trossen_arm::TrossenArmDriver> follower_driver_;

  /// @brief Configuration parameters
  Config cfg_;

  /// @brief Reusable joint state buffers to avoid reallocation each poll
  std::vector<double> act_d_, obs_d_;

  /// @brief Reusable robot output to avoid reallocation each poll for leader arm
  trossen_arm::RobotOutput leader_robot_output_;

  /// @brief Reusable robot output to avoid reallocation each poll for follower arm
  trossen_arm::RobotOutput follower_robot_output_;
  
  /// @brief Metadata for producer
  TeleopTrossenArmProducerMetadata metadata_;
};

} // namespace trossen::hw::arm

#endif // TROSSEN_SDK__HW__ARM__TELEOP_ARM_PRODUCER_HPP
