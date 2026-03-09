/**
 * @file realsense_camera_component.hpp
 * @brief Hardware component wrapper for RealSense-compatible cameras
 */

#ifndef TROSSEN_SDK__HW__CAMERA__REALSENSE_CAMERA_COMPONENT_HPP_
#define TROSSEN_SDK__HW__CAMERA__REALSENSE_CAMERA_COMPONENT_HPP_

#include <memory>
#include <string>

#include "librealsense2/rs.hpp"

#include "trossen_sdk/hw/hardware_component.hpp"

namespace trossen::hw::camera {

/**
 * @brief Hardware component for RealSense-compatible cameras
 *
 * Owns the rs2::pipeline and exposes it to push producers via get_hardware().
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
   * @brief Check if camera is opened and ready
   * @return true if camera pipeline and frame cache can be accessed
   */
  bool is_opened() const;

  /**
   * @brief Get the active RealSense pipeline
   * @return Shared pointer to rs2::pipeline (valid after configure())
   */
  std::shared_ptr<rs2::pipeline> get_hardware() const { return pipeline_; }

  /**
   * @brief Get depth scale factor (multiply Z16 raw value by this to get meters)
   * @return Depth scale in m/Z16 unit, 0 if depth not enabled
   */
  float get_depth_scale() const { return depth_scale_; }

  /**
   * @brief Get the use_depth config flag
   * @return true if depth stream was configured
   */
  bool get_use_depth() const { return use_depth_; }

  /**
   * @brief Get the align_depth_to_color config flag
   * @return true if depth should be aligned to color
   */
  bool get_align_depth_to_color() const { return align_depth_to_color_; }

private:
  /// @brief Realsense pipeline profile
  // Used for getting actual stream parameters after starting the pipeline
  rs2::pipeline_profile profile;

  /// @brief Camera serial number
  std::string serial_number = "unspecified";

  /// @brief Configured width
  int width_ = 640;

  /// @brief Configured height
  int height_ = 480;

  /// @brief Configured frame rate
  int fps_ = 30;

  /// @brief Configure use_depth flag
  bool use_depth_ = false;

  /// @brief Whether to align depth to color frame
  bool align_depth_to_color_ = true;

  /// @brief Shared pointer to the active pipeline (used by RealsensePushProducer)
  std::shared_ptr<rs2::pipeline> pipeline_;

  /// @brief Depth scale factor (meters per Z16 unit), 0 if depth not enabled
  float depth_scale_ = 0.0f;

  /// @brief Configure force_hardware_reset flag
  bool force_hardware_reset_ = false;
};

}  // namespace trossen::hw::camera

#endif  // TROSSEN_SDK__HW__CAMERA__REALSENSE_CAMERA_COMPONENT_HPP_
