/**
 * @file mock_joint_producer.hpp
 * @brief Mock joint state producer for instrumentation & drop diagnostics
 */

#ifndef TROSSEN_SDK__HW__ARM__TELEOP_MOCK_JOINT_PRODUCER_HPP
#define TROSSEN_SDK__HW__ARM__TELEOP_MOCK_JOINT_PRODUCER_HPP

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

namespace trossen::hw::arm {

/**
 * @brief Producer that emits synthetic joint states for testing and diagnostics.
 */
class TeleopMockJointStateProducer : public ::trossen::hw::PolledProducer {
public:
  /**
   * @brief Configuration parameters for TeleopMockJointStateProducer
   */
  struct Config {
    /// @brief Number of joints
    size_t num_joints{6};

    /// @brief Update rate in Hz
    double rate_hz{200.0};

    /// @brief Logical stream identifier
    std::string id{"mock"};

    /// @brief Amplitude of joint position sine wave oscillation
    double amplitude{1.0};
  };

  /**
   * @brief Diagnostics information for TeleopMockJointStateProducer
   */
  struct Diagnostics {
    /// @brief sequence gaps detected

    uint64_t gaps{0};

    /// @brief poll interval overruns
    uint64_t overruns{0};

    /// @brief running mean of poll intervals
    double avg_period_ms{0.0};

    /// @brief max observed poll interval
    double max_period_ms{0.0};
  };

  /**
   * @brief Metadata specific to TeleopMockJointStateProducer
   */
  struct TeleopMockJointStateProducerMetadata : public PolledProducer::ProducerMetadata {
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
   * @brief Construct a TeleopMockJointStateProducer
   */
  explicit TeleopMockJointStateProducer(Config cfg);

  /**
   * @brief Destructor
   */
  ~TeleopMockJointStateProducer() override = default;

  /**
   * @brief Poll the producer for new data and emit records via the callback
   *
   * @param emit Callback to invoke for each produced record
   */
  void poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;

  /**
   * @brief Get diagnostics information
   *
   * @return Diagnostics struct
   */
  Diagnostics diagnostics() const { return diag_; }

  /**
   * @brief Get configuration
   *
   * @return Config struct
   */
  const Config& config() const { return cfg_; }

  /**
   * @brief Get producer metadata
   *
   * @return const reference to ProducerMetadata
   */
  std::shared_ptr<ProducerMetadata> metadata() const override {
    return std::make_shared<TeleopMockJointStateProducerMetadata>(metadata_);
  }

private:
  /// @brief Configuration parameters
  Config cfg_;

  /// @brief Producer diagnostics
  Diagnostics diag_{};

  /// @brief Whether the producer has started
  bool started_{false};

  /// @brief Last tick time point
  std::chrono::steady_clock::time_point last_tick_{};

  /// @brief Producer metadata
  TeleopMockJointStateProducerMetadata metadata_;
};

}  // namespace trossen::hw::arm

#endif  // TROSSEN_SDK__HW__ARM__MOCK_JOINT_PRODUCER_HPP
