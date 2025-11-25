/**
 * @file mock_joint_producer.hpp
 * @brief Mock joint state producer for instrumentation & drop diagnostics
 */
#ifndef TROSSEN_SDK__HW__ARM__TELEOP_MOCK_JOINT_PRODUCER_HPP
#define TROSSEN_SDK__HW__ARM__TELEOP_MOCK_JOINT_PRODUCER_HPP

#include <vector>
#include <string>
#include <memory>
#include <chrono>

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

namespace trossen::hw::arm {

class TeleopMockJointStateProducer : public ::trossen::hw::PolledProducer {
public:
  struct Config {
    size_t num_joints{6};
    double rate_hz{200.0};
    std::string id{"mock"};
    double amplitude{1.0};
  };

  struct Diagnostics {
    uint64_t gaps{0};          // sequence gaps detected
    uint64_t overruns{0};      // poll interval overruns
    double avg_period_ms{0.0}; // running mean of poll intervals
    double max_period_ms{0.0}; // max observed poll interval
  };

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

  explicit TeleopMockJointStateProducer(Config cfg);
  ~TeleopMockJointStateProducer() override = default;

  void poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;

  Diagnostics diagnostics() const { return diag_; }
  const Config& config() const { return cfg_; }

  std::shared_ptr<ProducerMetadata> metadata() const override {
    return std::make_shared<TeleopMockJointStateProducerMetadata>(metadata_);
  }

private:
  Config cfg_;
  Diagnostics diag_{};
  bool started_{false};
  std::chrono::steady_clock::time_point last_tick_{};
  TeleopMockJointStateProducerMetadata metadata_;
};

} // namespace trossen::hw::arm

#endif  // TROSSEN_SDK__HW__ARM__MOCK_JOINT_PRODUCER_HPP
