/**
 * @file arm_producer_config.hpp
 * @brief Configuration for an arm joint-state producer
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__PRODUCERS__ARM_PRODUCER_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__PRODUCERS__ARM_PRODUCER_CONFIG_HPP_

#include "nlohmann/json.hpp"

namespace trossen::configuration {

/**
 * @brief Shared settings applied to all arm producers
 *
 * JSON format (under "producers.arms"):
 * {
 *   "poll_rate_hz": 30.0,
 *   "use_device_time": false
 * }
 */
struct ArmProducerConfig {
  /// @brief Polling frequency in Hz
  float poll_rate_hz{30.0f};

  /// @brief Use hardware device timestamp when available
  bool use_device_time{false};

  static ArmProducerConfig from_json(const nlohmann::json& j) {
    ArmProducerConfig c;
    if (j.contains("poll_rate_hz")) j.at("poll_rate_hz").get_to(c.poll_rate_hz);
    if (j.contains("use_device_time")) j.at("use_device_time").get_to(c.use_device_time);
    return c;
  }

  /// @brief Build the JSON config for ProducerRegistry::create(), given the stream id
  nlohmann::json to_json(const std::string& stream_id) const {
    return nlohmann::json{
      {"stream_id", stream_id},
      {"use_device_time", use_device_time}
    };
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__PRODUCERS__ARM_PRODUCER_CONFIG_HPP_
