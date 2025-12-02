/**
 * @file mock_joint_producer.hpp
 * @brief Mock joint state producer for instrumentation & drop diagnostics
 */
#ifndef TROSSEN_SDK__HW__ARM__MOCK_JOINT_PRODUCER_HPP
#define TROSSEN_SDK__HW__ARM__MOCK_JOINT_PRODUCER_HPP

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

namespace trossen::hw::arm {

class MockJointStateProducer : public ::trossen::hw::PolledProducer {
public:
  struct Config {
    size_t num_joints{6};
    double rate_hz{200.0};
    std::string id{"mock"};
    double amplitude{1.0};
  };

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

  struct MockJointStateProducerMetadata : public PolledProducer::ProducerMetadata {
    /// @brief Robot model
    std::string arm_model;

    /// @brief Joint names
    std::vector<std::string> joint_names;

    /// @brief Gripper type
    std::string gripper_type;

    /// @brief Get producer info as JSON
    /// @return JSON object containing producer information
    nlohmann::ordered_json   get_info() const override {
      // TODO(shantanuparab-tr): Implement JSON output when needed
      std::cout << "MockJointStateProducerMetadata: " << name << " (" << id << ") - "
                << description
                << ", Model: " << arm_model
                << ", Gripper: " << gripper_type << "\n";
      return nlohmann::ordered_json{};
    }
  };

  explicit MockJointStateProducer(Config cfg);
  ~MockJointStateProducer() override = default;

  void poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;

  Diagnostics diagnostics() const { return diag_; }
  const Config& config() const { return cfg_; }

  std::shared_ptr<ProducerMetadata> metadata() const override {
    return std::make_shared<MockJointStateProducerMetadata>(metadata_);
  }

private:
  Config cfg_;
  Diagnostics diag_{};
  bool started_{false};
  std::chrono::steady_clock::time_point last_tick_{};
  MockJointStateProducerMetadata metadata_;
};
}  // namespace trossen::hw::arm

#endif  // TROSSEN_SDK__HW__ARM__MOCK_JOINT_PRODUCER_HPP
