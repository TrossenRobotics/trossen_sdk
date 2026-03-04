/**
 * @file arm_producer.hpp
 * @brief Producer that emits joint states from a Trossen Arm via TrossenArmDriver.
 */

#ifndef TROSSEN_SDK__HW__ARM__ARM_PRODUCER_HPP
#define TROSSEN_SDK__HW__ARM__ARM_PRODUCER_HPP

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

namespace trossen::hw::arm {

/**
 * @brief Producer that emits joint states from a Trossen Arm via TrossenArmDriver.
 */
class TrossenArmProducer : public ::trossen::hw::PolledProducer {
public:
  /**
   * @brief Configuration parameters for TrossenArmProducer
   */
  struct Config {
    /// @brief Logical stream identifier (e.g. "arm_left")
    std::string stream_id{"arm"};

    /// @brief Prefer device timestamp if available
    bool use_device_time{true};
  };

  /**
   * @brief Metadata specific to TrossenArmProducer
   */
  struct TrossenArmProducerMetadata : public PolledProducer::ProducerMetadata {
    /// @brief Robot model
    std::string arm_model;

    /// @brief Joint names
    std::vector<std::string> joint_names;

    /// @brief Gripper type
    std::string gripper_type;

    /**
     * @brief Get producer info as JSON (LeRobot feature format)
     *
     * For a single-arm (solo) setup, the arm acts as both action and observation source.
     *
     * @return JSON object with "action" and "observation.state" feature entries
     */
    nlohmann::ordered_json get_info() const override {
      int n = static_cast<int>(joint_names.size());
      nlohmann::ordered_json features;
      features["action"]["dtype"] = "float32";
      features["action"]["shape"] = nlohmann::json::array({n});
      features["action"]["names"] = joint_names;
      features["observation.state"]["dtype"] = "float32";
      features["observation.state"]["shape"] = nlohmann::json::array({n});
      features["observation.state"]["names"] = joint_names;
      return features;
    }

    /**
     * @brief Get per-stream dataset metadata for MCAP recording
     *
     * @return JSON with "streams.<stream_id>.joint_names"
     */
    nlohmann::ordered_json get_stream_info() const override {
      nlohmann::ordered_json info;
      info["streams"][id]["joint_names"] = joint_names;
      return info;
    }
  };

  /**
   * @brief Construct a TrossenArmProducer from hardware component
   *
   * @param hardware Hardware component (must be TrossenArmComponent)
   * @param config JSON configuration with fields: stream_id, use_device_time
   * @throws std::invalid_argument if hardware is null or wrong type
   */
  TrossenArmProducer(
    std::shared_ptr<hw::HardwareComponent> hardware,
    const nlohmann::json& config);

  /**
   * @brief Destructor
   */
  ~TrossenArmProducer() override = default;

  /**
   * @brief Poll the driver for the latest joint states and emit a JointStateRecord
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
    return std::make_shared<TrossenArmProducerMetadata>(metadata_);
  }

private:
  /// @brief Shared pointer to the TrossenArmDriver instance
  std::shared_ptr<trossen_arm::TrossenArmDriver> driver_;

  /// @brief Configuration parameters
  Config cfg_;

  /// @brief Reusable joint state buffers to avoid reallocation each poll
  std::vector<double> pos_d_, vel_d_, eff_d_;

  /// @brief Reusable robot output to avoid reallocation each poll
  trossen_arm::RobotOutput robot_output_;

  /// @brief Producer metadata
  TrossenArmProducerMetadata metadata_;
};

}  // namespace trossen::hw::arm

#endif  // TROSSEN_SDK__HW__ARM__ARM_PRODUCER_HPP
