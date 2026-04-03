/**
 * @file producer_config.hpp
 * @brief Configuration for a single data producer
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__PRODUCERS__PRODUCER_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__PRODUCERS__PRODUCER_CONFIG_HPP_

#include <string>

#include "nlohmann/json.hpp"

namespace trossen::configuration {

/**
 * @brief Configuration for an individual producer instance
 *
 * Each producer entry in the config specifies the hardware component it reads from,
 * the producer type (which selects the ProducerRegistry factory), the stream ID used
 * for recording, the polling rate, and any type-specific settings.
 *
 * JSON format:
 * @code
 * {
 *   "type":            "trossen_arm",   // producer/hardware registry key
 *   "hardware_id":     "leader",        // references hardware config id
 *   "stream_id":       "leader",        // data stream name written to the backend
 *   "poll_rate_hz":    30.0,
 *   "use_device_time": false,
 *   "encoding":        "bgr8"           // camera producers only
 * }
 * @endcode
 *
 * Known types:
 *  - "trossen_arm"      - arm joint-state producer
 *  - "zed_camera"       - StereoLabs ZED stereo camera producer
 *  - "realsense_camera" - RealSense image producer
 *  - "opencv_camera"    - OpenCV/V4L2 image producer
 *  - "slate_base"       - SLATE mobile base velocity producer
 */
struct ProducerConfig {
  /// @brief Producer/hardware registry key (e.g. "trossen_arm", "realsense_camera")
  std::string type;

  /// @brief Id of the hardware component this producer reads from
  std::string hardware_id;

  /// @brief Stream identifier written to the recording backend
  std::string stream_id;

  /// @brief Polling frequency in Hz
  float poll_rate_hz{30.0f};

  /// @brief Use hardware device timestamp when available
  bool use_device_time{false};

  /// @brief Pixel encoding - camera producers only (e.g. "bgr8", "rgb8", "mono8")
  std::string encoding{"bgr8"};

  static ProducerConfig from_json(const nlohmann::json& j) {
    ProducerConfig c;
    if (j.contains("type"))            j.at("type").get_to(c.type);
    if (j.contains("hardware_id"))     j.at("hardware_id").get_to(c.hardware_id);
    if (j.contains("stream_id"))       j.at("stream_id").get_to(c.stream_id);
    if (j.contains("poll_rate_hz"))    j.at("poll_rate_hz").get_to(c.poll_rate_hz);
    if (j.contains("use_device_time")) j.at("use_device_time").get_to(c.use_device_time);
    if (j.contains("encoding"))        j.at("encoding").get_to(c.encoding);
    return c;
  }

  /// @brief Build JSON for ProducerRegistry::create() - arm and mobile-base producers
  nlohmann::json to_registry_json() const {
    return nlohmann::json{
      {"stream_id",       stream_id},
      {"use_device_time", use_device_time}
    };
  }

  /// @brief Build JSON for ProducerRegistry::create() - camera producers
  nlohmann::json to_registry_json(int width, int height, int fps) const {
    return nlohmann::json{
      {"stream_id",       stream_id},
      {"encoding",        encoding},
      {"use_device_time", use_device_time},
      {"width",           width},
      {"height",          height},
      {"fps",             fps}
    };
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__PRODUCERS__PRODUCER_CONFIG_HPP_
