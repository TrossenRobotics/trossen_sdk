/**
 * @file mobile_base_producer_config.hpp
 * @brief Configuration for a mobile base velocity producer
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__PRODUCERS__MOBILE_BASE_PRODUCER_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__PRODUCERS__MOBILE_BASE_PRODUCER_CONFIG_HPP_

#include <string>

#include "nlohmann/json.hpp"

namespace trossen::configuration {

/**
 * @brief Settings for the mobile base producer
 *
 * JSON format (under "producers.mobile_base"):
 * {
 *   "poll_rate_hz": 30.0,
 *   "use_device_time": false
 * }
 */
struct MobileBaseProducerConfig {
  /// @brief Polling frequency in Hz
  float poll_rate_hz{30.0f};

  /// @brief Use hardware device timestamp when available
  bool use_device_time{false};

  static MobileBaseProducerConfig from_json(const nlohmann::json& j) {
    MobileBaseProducerConfig c;
    if (j.contains("poll_rate_hz")) j.at("poll_rate_hz").get_to(c.poll_rate_hz);
    if (j.contains("use_device_time")) j.at("use_device_time").get_to(c.use_device_time);
    return c;
  }

  /// @brief Build the JSON config for ProducerRegistry::create()
  nlohmann::json to_json(const std::string& stream_id) const {
    return nlohmann::json{
      {"stream_id", stream_id},
      {"use_device_time", use_device_time}
    };
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__PRODUCERS__MOBILE_BASE_PRODUCER_CONFIG_HPP_
