/**
 * @file rivet_producer.hpp
 * @brief Producer that emits bimanual joint states and base velocities from Rivet.
 */

#ifndef TROSSEN_SDK__HW__COMPOSITE__RIVET_PRODUCER_HPP_
#define TROSSEN_SDK__HW__COMPOSITE__RIVET_PRODUCER_HPP_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

namespace trossen::hw::rivet {

// Retained by the producer (as a shared_ptr member) to read the base's last
// commanded velocities each poll. The full definition is included in the
// .cpp; a forward declaration suffices here since the member is only a
// shared_ptr.
class RivetComponent;

/**
 * @brief Producer that emits bimanual joint states and base velocities from Rivet.
 *
 */
class RivetProducer : public ::trossen::hw::PolledProducer {
public:
  /**
   * @brief Configuration parameters for RivetProducer
   */
  struct Config {
    /// @brief Logical stream identifier
    std::string stream_id{"rivet"};

    /// @brief Prefer device timestamp if available
    bool use_device_time{true};
  };

  /**
   * @brief Metadata specific to RivetProducer
   */
  struct RivetProducerMetadata : public PolledProducer::ProducerMetadata {
    /// @brief Robot model
    std::string arm_model;

    /// @brief Left arm joint names (gripper included)
    std::vector<std::string> left_joint_names;

    /// @brief Right arm joint names (gripper included)
    std::vector<std::string> right_joint_names;

    /// @brief Gripper type
    std::string gripper_type;

    /**
     * @brief Get producer info as JSON (LeRobot feature format)
     *
     * @return JSON object with "action" and "observation.state" feature entries
     */
    nlohmann::ordered_json get_info() const override {
      std::vector<std::string> names = left_joint_names;
      names.insert(names.end(), right_joint_names.begin(), right_joint_names.end());
      names.insert(names.end(), {"base_vel_x", "base_vel_y", "base_vel_r", "base_vel_lift"});
      int n = static_cast<int>(names.size());
      nlohmann::ordered_json features;
      features["action"]["dtype"] = "float32";
      features["action"]["shape"] = nlohmann::json::array({n});
      features["action"]["names"] = names;
      features["observation.state"]["dtype"] = "float32";
      features["observation.state"]["shape"] = nlohmann::json::array({n});
      features["observation.state"]["names"] = names;
      return features;
    }

    /**
     * @brief Get per-stream dataset metadata for MCAP recording
     *
     * @return JSON with "streams.<stream_id>.{left,right}_joint_names" and "has_mobile_base"
     */
    nlohmann::ordered_json get_stream_info() const override {
      nlohmann::ordered_json info;
      info["streams"][id]["left_joint_names"] = left_joint_names;
      info["streams"][id]["right_joint_names"] = right_joint_names;
      info["has_mobile_base"] = true;
      return info;
    }
  };

  /**
   * @brief Construct a RivetProducer from hardware component
   *
   * @param hardware Hardware component (must be RivetComponent)
   * @param config JSON configuration with fields: stream_id, use_device_time
   * @throws std::invalid_argument if hardware is null, wrong type, or missing a driver
   */
  RivetProducer(
    std::shared_ptr<hw::HardwareComponent> hardware,
    const nlohmann::json& config);

  /**
   * @brief Destructor
   */
  ~RivetProducer() override = default;

  /**
   * @brief Poll both arm drivers for joint states, read the base's last
   * commanded velocities, and emit a RivetRecord
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
    return std::make_shared<RivetProducerMetadata>(metadata_);
  }

private:
  /// @brief Shared pointers to the left/right arm driver instances
  std::shared_ptr<trossen_arm::TrossenArmDriver> left_driver_;
  std::shared_ptr<trossen_arm::TrossenArmDriver> right_driver_;

  /// @brief Owning component, retained so poll() can read the base's last
  /// commanded velocities (trossen_base exposes no measured velocity feedback).
  std::shared_ptr<RivetComponent> rivet_component_;

  /// @brief Configuration parameters
  Config cfg_;

  /// @brief Producer metadata
  RivetProducerMetadata metadata_;
};

}  // namespace trossen::hw::rivet

#endif  // TROSSEN_SDK__HW__COMPOSITE__RIVET_PRODUCER_HPP_