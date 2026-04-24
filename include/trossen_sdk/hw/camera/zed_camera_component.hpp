/**
 * @file zed_camera_component.hpp
 * @brief Hardware component wrapper for StereoLabs ZED stereo cameras
 *
 * Owns an sl::Camera instance and exposes it to the ZedPushProducer via
 * get_hardware().  The ZED SDK is CUDA-based; sl::Camera::open() allocates
 * GPU context, so each component maps to exactly one physical camera.
 *
 * Build gate: compiled only when TROSSEN_ENABLE_ZED=ON (CMake option).
 */

#ifndef TROSSEN_SDK__HW__CAMERA__ZED_CAMERA_COMPONENT_HPP_
#define TROSSEN_SDK__HW__CAMERA__ZED_CAMERA_COMPONENT_HPP_

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "sl/Camera.hpp"

#include "trossen_sdk/hw/discovery_registry.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"

namespace trossen::hw::camera {

/**
 * @brief Hardware component for StereoLabs ZED stereo cameras (ZED 2, ZED 2i,
 *        ZED Mini, ZED X, ZED X Mini).
 *
 * Wraps sl::Camera; the camera is opened during configure() and remains open
 * until the component is destroyed.  A shared_ptr<sl::Camera> is exposed so
 * that the push producer can call grab() / retrieveImage() / retrieveMeasure()
 * on the same session.
 *
 * Registered in HardwareRegistry as @c "zed_camera".
 */
class ZedCameraComponent : public HardwareComponent {
public:
  /**
   * @brief Constructor
   *
   * @param identifier Component identifier (e.g. "zed_0", "depth_cam")
   */
  explicit ZedCameraComponent(const std::string& identifier)
    : HardwareComponent(identifier) {}
  ~ZedCameraComponent() override;

  /**
   * @brief Configure and open the ZED camera
   *
   * Expected JSON keys (all optional except serial_number):
   * @code
   * {
   *   "serial_number":  "12345678",
   *   "resolution":     "HD720",       // HD2K | HD1080 | HD720 | VGA | AUTO
   *   "fps":            30,
   *   "depth_mode":     "NEURAL",      // NONE | NEURAL_LIGHT | NEURAL | NEURAL_PLUS
   *   "use_depth":      true,
   *   "extra": { ... }                 // passthrough JSON for advanced settings
   * }
   * @endcode
   *
   * @param config JSON configuration
   * @throws std::runtime_error on open failure
   */
  void configure(const nlohmann::json& config) override;

  std::string get_type() const override { return "zed_camera"; }
  nlohmann::json get_info() const override;

  /// @brief Get camera serial number
  std::string get_serial_number() const { return serial_number_; }

  /// @brief Check if the camera is opened and ready
  bool is_opened() const;

  /// @brief Get negotiated frame width (after open)
  int get_width() const { return width_; }

  /// @brief Get negotiated frame height (after open)
  int get_height() const { return height_; }

  /// @brief Get negotiated frame rate
  int get_fps() const { return fps_; }

  /// @brief Get the underlying sl::Camera handle (valid after configure())
  std::shared_ptr<sl::Camera> get_hardware() const { return camera_; }

  /// @brief Whether depth was requested at configure time
  bool get_use_depth() const { return use_depth_; }

  /// @brief Get the configured depth mode string (for logging)
  const std::string& get_depth_mode_str() const { return depth_mode_str_; }

  /**
   * @brief Enumerate connected ZED cameras. Not yet implemented.
   *
   * Currently a no-op stub: prints a "not yet implemented" notice and returns
   * an empty list. Intended to mirror the RealSense/OpenCV discovery contract
   * once the ZED preview-capture path is wired up.
   *
   * @param output_dir Unused. Reserved for future preview-JPEG output.
   * @return Empty vector.
   */
  static std::vector<DiscoveredHardware> find(const std::filesystem::path& output_dir);

private:
  /// @brief Parse a depth mode string to sl::DEPTH_MODE
  static sl::DEPTH_MODE parse_depth_mode(const std::string& mode_str);

  /// @brief Parse a resolution string to sl::RESOLUTION
  static sl::RESOLUTION parse_resolution(const std::string& res_str);

  std::shared_ptr<sl::Camera> camera_;
  std::string serial_number_{"unspecified"};
  int width_{0};
  int height_{0};
  int fps_{0};
  bool use_depth_{false};
  std::string depth_mode_str_{"NONE"};
};

}  // namespace trossen::hw::camera

#endif  // TROSSEN_SDK__HW__CAMERA__ZED_CAMERA_COMPONENT_HPP_
