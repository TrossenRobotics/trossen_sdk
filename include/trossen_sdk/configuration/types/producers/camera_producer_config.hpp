/**
 * @file camera_producer_config.hpp
 * @brief Configuration for a camera image producer
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__PRODUCERS__CAMERA_PRODUCER_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__PRODUCERS__CAMERA_PRODUCER_CONFIG_HPP_

#include <string>

#include "nlohmann/json.hpp"

namespace trossen::configuration {

/**
 * @brief Shared settings applied to all camera producers
 *
 * JSON format (under "producers.cameras"):
 * {
 *   "poll_rate_hz": 30.0,
 *   "encoding": "bgr8",
 *   "use_device_time": true
 * }
 */
struct CameraProducerConfig {
  /// @brief Polling frequency in Hz
  float poll_rate_hz{30.0f};

  /// @brief Pixel encoding passed to the producer (e.g. "bgr8", "rgb8", "mono8")
  std::string encoding{"bgr8"};

  /// @brief Use hardware device timestamp when available
  bool use_device_time{true};

  static CameraProducerConfig from_json(const nlohmann::json& j) {
    CameraProducerConfig c;
    if (j.contains("poll_rate_hz")) j.at("poll_rate_hz").get_to(c.poll_rate_hz);
    if (j.contains("encoding")) j.at("encoding").get_to(c.encoding);
    if (j.contains("use_device_time")) j.at("use_device_time").get_to(c.use_device_time);
    return c;
  }

  /// @brief Build the JSON config for ProducerRegistry::create(), given camera info
  nlohmann::json to_json(
    const std::string& stream_id,
    int width,
    int height,
    int fps) const
  {
    return nlohmann::json{
      {"stream_id", stream_id},
      {"encoding", encoding},
      {"use_device_time", use_device_time},
      {"width", width},
      {"height", height},
      {"fps", fps}
    };
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__PRODUCERS__CAMERA_PRODUCER_CONFIG_HPP_
