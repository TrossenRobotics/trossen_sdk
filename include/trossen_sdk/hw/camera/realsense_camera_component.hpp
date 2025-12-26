/**
 * @file realsense_camera_component.hpp
 * @brief Hardware component wrapper for RealSense-compatible cameras
 */

#ifndef TROSSEN_SDK__HW__CAMERA__REALSENSE_CAMERA_COMPONENT_HPP_
#define TROSSEN_SDK__HW__CAMERA__REALSENSE_CAMERA_COMPONENT_HPP_

#include <memory>
#include <string>

#include "trossen_sdk/hw/camera/realsense_frame_cache.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"

namespace trossen::hw::camera {

/**
 * @brief Hardware component for Realsense-compatible cameras
 *
 * Owns RealsenseFrameCache instance and provides JSON configuration.
 */
class RealsenseCameraComponent : public HardwareComponent {
public:
  /**
   * @brief Constructor
   *
   * @param identifier Component identifier (e.g., "camera_0", "wrist_cam")
   */
  explicit RealsenseCameraComponent(const std::string& identifier)
    : HardwareComponent(identifier) {}
  ~RealsenseCameraComponent() override;

  /**
   * @brief Configure the camera from JSON
   *
   * Expected JSON format:
   * {
   *   "serial_number": "012345678",
   *   "width": 640,
   *   "height": 480,
   *   "fps": 30,
   *   "use_depth": true,
   *   "force_hardware_reset": false
   * }
   *
   * @param config JSON configuration object
   * @throws std::runtime_error if configuration fails
   */
  void configure(const nlohmann::json& config) override;

  /**
   * @brief Get the type string for this hardware component
   *
   * @return Type identifier
   */
  std::string get_type() const override { return "realsense_camera"; }

  /**
   * @brief Get human-readable component information
   *
   * @return JSON object with component details
   */
  nlohmann::json get_info() const override;

  /**
   * @brief Get the camera serial number
   * @return Serial number of the camera
   */
  std::string get_serial_number() const { return serial_number; }

  /**
   * @brief Get the underlying hardware (RealsenseFrameCache) instance
   *
   * @return Shared pointer to RealsenseFrameCache (for producer access)
   */
  std::shared_ptr<RealsenseFrameCache> get_hardware() { return frame_cache_; }

  /**
   * @brief Check if camera is opened and ready
   * @return true if camera pipeline and frame cache can be accessed
   */
  bool is_opened() const;

private:
  /// @brief Underlying RealsenseFrameCache for Realsense camera
  std::shared_ptr<RealsenseFrameCache> frame_cache_;

  /// @brief Realsense pipeline profile
  // Used for getting actual stream parameters after starting the pipeline
  rs2::pipeline_profile profile;

  /// @brief Camera device index
  std::string serial_number = "012345678";

  /// @brief Configured width
  int width_ = 640;

  /// @brief Configured height
  int height_ = 480;

  /// @brief Configured frame rate
  int fps_ = 30;

  /// @brief Configure use_depth flag
  bool use_depth_ = false;

  /// @brief Configure force_hardware_reset flag
  bool force_hardware_reset_ = false;
};

}  // namespace trossen::hw::camera

#endif  // TROSSEN_SDK__HW__CAMERA__REALSENSE_CAMERA_COMPONENT_HPP_
