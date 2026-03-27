/**
 * @file camera_config.hpp
 * @brief Configuration for a camera hardware component
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__CAMERA_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__CAMERA_CONFIG_HPP_

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace trossen::configuration {

/**
 * @brief Configuration for a single camera hardware component
 *
 * The @p type field selects which hardware component is created via HardwareRegistry.
 * Supported types: @c "realsense_camera", @c "opencv_camera", @c "zed_camera".
 *
 * Common fields across all camera types:
 *   - @c id   — logical identifier used as stream_id (e.g. "camera_0")
 *   - @c type — hardware registry key
 *   - @c fps  — capture frame rate
 *
 * Resolution configuration varies by camera type:
 *
 * **RealSense / OpenCV** — set resolution via @c width and @c height in pixels.
 * Any resolution the hardware supports can be requested; the driver will
 * negotiate the closest match. Common examples: 640x480, 1280x720, 1920x1080.
 *
 * **ZED** — set resolution via the @c "resolution" string in the @c extra
 * passthrough field. Supported values (default: "HD720"):
 *   - "HD2K"   — 2208x1242
 *   - "HD1200" — 1920x1200
 *   - "HD1080" — 1920x1080
 *   - "HD720"  — 1280x720
 *   - "SVGA"   — 960x600
 *   - "VGA"    — 672x376
 *   - "AUTO"   — selected by the SDK based on the camera model
 *
 * Depth support:
 *   - **RealSense** — enable with @c use_depth. Uses the same width/height as color.
 *   - **ZED** — enable with @c use_depth and set @c "depth_mode" in extra.
 *     Supported depth modes: "NEURAL_LIGHT", "NEURAL", "NEURAL_PLUS", "NONE".
 *     Legacy modes "PERFORMANCE", "QUALITY", "ULTRA" are accepted but deprecated
 *     in ZED SDK 5.x — prefer "NEURAL_LIGHT", "NEURAL", "NEURAL_PLUS" respectively.
 *   - **OpenCV** — depth not supported.
 *
 * Camera-type-specific fields:
 *   - @c serial_number — device serial (RealSense: string, ZED: numeric, both accepted)
 *   - @c device_index  — V4L2 device index (OpenCV only)
 *   - @c backend       — capture backend, "v4l2" or "any" (OpenCV only)
 *   - @c force_hardware_reset — reset device on open (RealSense only)
 *   - @c extra         — passthrough JSON for fields not modelled above (e.g. ZED
 *                         "resolution", "depth_mode"); merged into the config sent
 *                         to HardwareComponent::configure()
 *
 * JSON examples:
 * @code
 * // RealSense
 * {
 *   "id": "camera_0", "type": "realsense_camera",
 *   "serial_number": "128422271347",
 *   "width": 640, "height": 480, "fps": 30,
 *   "use_depth": false, "force_hardware_reset": false
 * }
 *
 * // OpenCV
 * {
 *   "id": "camera_0", "type": "opencv_camera",
 *   "device_index": 0,
 *   "width": 640, "height": 480, "fps": 30,
 *   "backend": "v4l2"
 * }
 *
 * // ZED
 * {
 *   "id": "zed_0", "type": "zed_camera",
 *   "serial_number": "12345678", "fps": 30,
 *   "resolution": "HD720",
 *   "use_depth": true, "depth_mode": "NEURAL"
 * }
 * @endcode
 */
struct CameraConfig {
  /// @brief Hardware registry key - "realsense_camera", "opencv_camera", or "zed_camera"
  std::string type{"realsense_camera"};

  /// @brief Logical camera identifier used as stream_id (e.g. "camera_0", "wrist_cam")
  std::string id{"camera_0"};

  // TODO(shantanuparab-tr): Unify serial_number and device_index into a
  // single device identifier field (serial string or numeric index).

  /// @brief Device serial number (RealSense: string, ZED: numeric — both accepted)
  std::string serial_number{""};

  /// @brief OpenCV device index (OpenCV only)
  int device_index{0};

  /// @brief OpenCV capture backend - "v4l2" or "any" (OpenCV only)
  std::string backend{""};

  // TODO(shantanuparab-tr): Unify resolution configuration across camera types. Consider accepting
  //       a resolution string (e.g. "720x480", "HD720", "SVGA") that maps to
  //       width/height for RealSense/OpenCV and to the native enum for ZED.

  /// @brief Capture width in pixels (RealSense / OpenCV; ZED uses "resolution" in extra)
  int width{640};

  /// @brief Capture height in pixels (RealSense / OpenCV; ZED uses "resolution" in extra)
  int height{480};

  /// @brief Capture frame rate
  int fps{30};

  /// @brief Enable depth stream alongside color (RealSense / ZED)
  bool use_depth{false};

  /// @brief Force hardware reset on open (RealSense only)
  bool force_hardware_reset{false};

  /// @brief Passthrough JSON for hardware-specific settings not modelled above
  /// (e.g. ZED "resolution", "depth_mode").  Merged into to_json() output so
  /// that HardwareComponent::configure() receives them transparently.
  nlohmann::json extra{};

  static CameraConfig from_json(const nlohmann::json& j) {
    CameraConfig c;
    if (j.contains("type")) j.at("type").get_to(c.type);
    if (j.contains("id")) j.at("id").get_to(c.id);
    // Accept both string and numeric serial_number (ZED uses numeric SNs)
    if (j.contains("serial_number")) {
      if (j["serial_number"].is_number_integer()) {
        c.serial_number = std::to_string(j["serial_number"].get<uint64_t>());
      } else {
        j.at("serial_number").get_to(c.serial_number);
      }
    }
    if (j.contains("device_index")) j.at("device_index").get_to(c.device_index);
    if (j.contains("backend")) j.at("backend").get_to(c.backend);
    if (j.contains("width")) j.at("width").get_to(c.width);
    if (j.contains("height")) j.at("height").get_to(c.height);
    if (j.contains("fps")) j.at("fps").get_to(c.fps);
    if (j.contains("use_depth")) j.at("use_depth").get_to(c.use_depth);
    if (j.contains("force_hardware_reset"))
      j.at("force_hardware_reset").get_to(c.force_hardware_reset);

    // Collect any keys not explicitly parsed above into `extra`.
    // WARNING: if you add a new known key above, you must also add it to the
    // known_keys list below — otherwise it will be silently duplicated into
    // the `extra` passthrough and may confuse downstream components.
    // TODO(shantanuparab-tr): Replace known_keys list with a parse-and-erase pattern — copy j,
    //       erase each key as it is parsed, then assign the remainder to extra.
    //       This eliminates the need to maintain a separate known_keys list.
    static const std::vector<std::string> known_keys = {
      "type", "id", "serial_number", "device_index", "backend",
      "width", "height", "fps", "use_depth", "force_hardware_reset"
    };
    for (auto it = j.begin(); it != j.end(); ++it) {
      bool is_known = false;
      for (const auto& k : known_keys) {
        if (it.key() == k) {
          is_known = true;
          break;
        }
      }
      if (!is_known) {
        c.extra[it.key()] = it.value();
      }
    }
    return c;
  }

  /// @brief Produce JSON suitable for HardwareComponent::configure()
  nlohmann::json to_json() const {
    nlohmann::json j{
      {"serial_number", serial_number},
      {"device_index", device_index},
      {"width", width},
      {"height", height},
      {"fps", fps},
      {"use_depth", use_depth},
      {"force_hardware_reset", force_hardware_reset}
    };
    if (!backend.empty()) {
      j["backend"] = backend;
    }
    // Merge passthrough keys (e.g. "resolution", "depth_mode" for ZED)
    if (!extra.is_null() && extra.is_object()) {
      j.merge_patch(extra);
    }
    return j;
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__CAMERA_CONFIG_HPP_
