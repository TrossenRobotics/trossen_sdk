/**
 * @file zed_camera_component.hpp
 * @brief Hardware component for ZED cameras
 */

#ifndef TROSSEN_SDK__HW__CAMERA__ZED_CAMERA_COMPONENT_HPP
#define TROSSEN_SDK__HW__CAMERA__ZED_CAMERA_COMPONENT_HPP

#include <memory>
#include <string>

#include "trossen_sdk/hw/camera/zed_frame_cache.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"

namespace trossen::hw::camera {

/**
 * @brief Hardware component for ZED cameras
 *
 * This component manages the lifecycle of a ZED camera and provides
 * a frame cache for use by color and depth producers.
 */
class ZedCameraComponent : public HardwareComponent {
public:
  /**
   * @brief Construct ZED camera component
   *
   * @param identifier Unique identifier for this camera instance
   */
  explicit ZedCameraComponent(std::string identifier)
    : HardwareComponent(std::move(identifier)) {}

  /**
   * @brief Destructor closes camera if open
   */
  ~ZedCameraComponent() override;

  /**
   * @brief Configure camera from JSON
   *
   * Expected JSON structure:
   * {
   *   "serial_number": 0,          // Optional, 0 = use any camera
   *   "resolution": "SVGA",        // VGA, SVGA, HD720, HD1080, HD2K
   *   "fps": 30,                   // Camera frame rate
   *   "use_depth": false,          // Enable depth computation
   *   "depth_mode": "NONE"         // NONE, PERFORMANCE, QUALITY, ULTRA, NEURAL
   * }
   *
   * @param config JSON configuration object
   */
  void configure(const nlohmann::json& config) override;

  /**
   * @brief Get the type string for this hardware component
   *
   * @return Type identifier "zed_camera"
   */
  std::string get_type() const override { return "zed_camera"; }

  /**
   * @brief Get camera info as JSON
   *
   * @return JSON object with camera details
   */
  nlohmann::json get_info() const override;

  /**
   * @brief Check if camera is opened
   *
   * @return true if camera is opened, false otherwise
   */
  bool is_opened() const;

  /**
   * @brief Get frame cache for use by producers
   *
   * @return Shared pointer to frame cache
   */
  std::shared_ptr<ZedFrameCache> get_hardware() const {
    return frame_cache_;
  }

  /**
   * @brief Get camera serial number
   *
   * @return Serial number of the camera
   */
  int get_serial_number() const { return serial_number_; }

  /**
   * @brief Get image width
   *
   * @return Width in pixels
   */
  int get_width() const { return width_; }

  /**
   * @brief Get image height
   *
   * @return Height in pixels
   */
  int get_height() const { return height_; }

private:
  /// @brief ZED camera instance
  std::shared_ptr<sl::Camera> camera_;

  /// @brief Frame cache (shared between color and depth producers)
  std::shared_ptr<ZedFrameCache> frame_cache_;

  /// @brief Camera serial number (0 = use any)
  int serial_number_{0};

  /// @brief Camera resolution
  sl::RESOLUTION resolution_{sl::RESOLUTION::SVGA};

  /// @brief Camera FPS
  int fps_{30};

  /// @brief Whether to use depth
  bool use_depth_{false};

  /// @brief Depth computation mode
  sl::DEPTH_MODE depth_mode_{sl::DEPTH_MODE::NONE};

  /// @brief Actual image width (from camera calibration)
  int width_{0};

  /// @brief Actual image height (from camera calibration)
  int height_{0};
};

}  // namespace trossen::hw::camera

#endif  // TROSSEN_SDK__HW__CAMERA__ZED_CAMERA_COMPONENT_HPP
