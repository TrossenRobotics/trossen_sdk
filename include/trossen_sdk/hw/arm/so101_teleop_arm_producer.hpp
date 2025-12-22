#ifndef TROSSEN_SDK__HW__ARM__SO101_TELEOP_ARM_PRODUCER_HPP
#define TROSSEN_SDK__HW__ARM__SO101_TELEOP_ARM_PRODUCER_HPP

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "trossen_sdk/hw/arm/so101_arm_driver.hpp"
#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

namespace trossen::hw::arm {

/**
 * @brief Producer that emits teleoperation joint states from leader and follower SO-101 Arms.
 */
class TeleopSO101ArmProducer : public ::trossen::hw::PolledProducer {
public:
  /**
   * @brief Configuration parameters for TeleopSO101ArmProducer
   */
  struct Config {
    /// @brief Logical stream identifier (e.g. "arm_left")
    std::string stream_id{"teleop_so101_arm"};

    /// @brief Prefer device timestamp if available
    bool use_device_time{false};
  };

  /**
   * @brief Metadata specific to TeleopSO101ArmProducer
   */
  struct TeleopSO101ArmProducerMetadata : public PolledProducer::ProducerMetadata {
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

    /**
     * @brief Get producer info as JSON
     *
     * @return JSON object containing producer information
     */
    nlohmann::ordered_json get_info() const override {
      nlohmann::ordered_json features;
      nlohmann::ordered_json action;
      action["dtype"] = action_dtype;
      action["shape"] = {static_cast<int>(action_feature_names.size())};
      action["names"] = action_feature_names;

      nlohmann::ordered_json observation_state;
      observation_state["dtype"] = observation_dtype;
      observation_state["shape"] = {
          static_cast<int>(observation_feature_names.size())};
      observation_state["names"] = observation_feature_names;
      features["action"] = action;
      features["observation.state"] = observation_state;
      return features;
    }
  };

  /**
   * @brief Construct a TeleopSO101ArmProducer
   *
   * @param leader Shared pointer to an initialized SO101ArmDriver instance (leader end effector)
   * @param follower Shared pointer to an initialized SO101ArmDriver instance (follower end effector)
   * @param cfg Configuration parameters
   */
  TeleopSO101ArmProducer(
    std::shared_ptr<SO101ArmDriver> leader,
    std::shared_ptr<SO101ArmDriver> follower,
    Config cfg);

  /**
   * @brief Destructor
   */
  ~TeleopSO101ArmProducer() override = default;

  /**
   * @brief Poll the leader and follower for the latest joint states and emit a TeleopJointStateRecord
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
    return std::make_shared<TeleopSO101ArmProducerMetadata>(metadata_);
  }

private:
  /// @brief Shared pointer to the SO101ArmDriver instance (leader end effector)
  std::shared_ptr<SO101ArmDriver> leader_;

  /// @brief Shared pointer to the SO101ArmDriver instance (follower end effector)
  std::shared_ptr<SO101ArmDriver> follower_;

  /// @brief Configuration parameters
  Config cfg_;

  /// @brief Metadata for producer
  TeleopSO101ArmProducerMetadata metadata_;
};

}  // namespace trossen::hw::arm

#endif  // TROSSEN_SDK__HW__ARM__SO101_TELEOP_ARM_PRODUCER_HPP
