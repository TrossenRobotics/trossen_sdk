/**
 * @file opencv_camera_component.hpp
 * @brief Hardware component wrapper for OpenCV-compatible cameras
 */

#ifndef TROSSEN_SDK__HW__CAMERA__OPENCV_CAMERA_COMPONENT_HPP_
#define TROSSEN_SDK__HW__CAMERA__OPENCV_CAMERA_COMPONENT_HPP_

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "opencv2/core.hpp"
#include "opencv2/videoio.hpp"

#include "trossen_sdk/hw/discovery_registry.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"

namespace trossen::hw::camera {

/**
 * @brief Hardware component for OpenCV-compatible cameras
 *
 * Owns cv::VideoCapture instance and provides JSON configuration.
 */
class OpenCvCameraComponent : public HardwareComponent {
public:
  /**
   * @brief Constructor
   *
   * @param identifier Component identifier (e.g., "camera_0", "wrist_cam")
   */
  explicit OpenCvCameraComponent(const std::string& identifier)
    : HardwareComponent(identifier) {}
  ~OpenCvCameraComponent() override;

  /**
   * @brief Configure the camera from JSON
   *
   * Expected JSON format:
   * {
   *   "device_index": 0,
   *   "width": 640,
   *   "height": 480,
   *   "fps": 30,
   *   "backend": "v4l2",      // optional, defaults to auto
   *   "warmup_frames": 10     // optional, number of frames to discard after opening
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
  std::string get_type() const override { return "opencv_camera"; }

  /**
   * @brief Get human-readable component information
   *
   * @return JSON object with component details
   */
  nlohmann::json get_info() const override;

  /**
   * @brief Get the camera device index
   * @return Device index used for VideoCapture
   */
  int get_device_index() const { return device_index_; }

  /**
   * @brief Get the underlying hardware (VideoCapture) instance
   *
   * @return Shared pointer to VideoCapture (for producer access)
   */
  std::shared_ptr<cv::VideoCapture> get_hardware() { return capture_; }

  /**
   * @brief Check if camera is opened and ready
   * @return true if VideoCapture is open
   */
  bool is_opened() const;

  /**
   * @brief Enumerate V4L2 video devices accessible through OpenCV.
   *
   * Probes @c /dev/video0 .. @c /dev/video63, opens each openable node, reads
   * a stable frame, and writes it as @c opencv_<index>.jpg inside @p output_dir.
   *
   * @param output_dir Directory to write preview JPEGs into. Must already exist.
   * @return One @c DiscoveredHardware per openable V4L2 device. @c details
   *         carries @c width / @c height / @c fps / @c preview_path; @c ok
   *         reflects preview-write success.
   */
  static std::vector<DiscoveredHardware> find(const std::filesystem::path& output_dir);

private:
  /// @brief Underlying VideoCapture device
  std::shared_ptr<cv::VideoCapture> capture_;

  /// @brief Camera device index
  int device_index_ = 0;

  /// @brief Configured width
  int width_ = 640;

  /// @brief Configured height
  int height_ = 480;

  /// @brief Configured frame rate
  int fps_ = 30;

  /// @brief OpenCV backend API (e.g., CAP_V4L2, CAP_ANY)
  int backend_ = cv::CAP_ANY;

  /// @brief Number of warmup frames to discard after opening
  int warmup_frames_ = 0;
};

}  // namespace trossen::hw::camera

#endif  // TROSSEN_SDK__HW__CAMERA__OPENCV_CAMERA_COMPONENT_HPP_
