#ifndef TROSSEN_SDK__HW__CAMERA__OPENCV_PRODUCER_HPP
#define TROSSEN_SDK__HW__CAMERA__OPENCV_PRODUCER_HPP

#include <functional>
#include <memory>
#include <string>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

namespace trossen::hw::camera {

class OpenCvCameraProducer : public ::trossen::hw::PolledProducer {
public:
  struct Config {
    /// @brief Index of the camera device (0, 1, etc.)
    int device_index = 0;

    /// @brief Logical stream identifier (e.g. "camera0")
    std::string stream_id = "camera0";

    /// @brief Desired output image encoding (e.g. "bgr8", "rgb8", "mono8")
    std::string encoding = "bgr8";

    /// @brief Desired image width (pixels). 0 = leave default
    int width = 0;   //

    /// @brief Desired image height (pixels). 0 = leave default
    int height = 0;

    /// @brief Desired frame rate (fps). 0 = leave default
    int fps = 0;

    /// @brief Prefer device timestamp if available
    bool use_device_time = true;

    /// @brief Seconds to warm up (discard frames) after device open before emitting
    double warmup_seconds = 0.0;
  };

  /**
   * @brief Construct an OpenCvCameraProducer
   *
   * @param cfg Configuration parameters
   */
  explicit OpenCvCameraProducer(Config cfg);

  /**
   * @brief Destructor
   */
  ~OpenCvCameraProducer() override;

  /**
   * @brief Perform a blocking warmup (open device if needed, discard frames for warmup_seconds)
   *
   * @return true on success
   */
  bool warmup();

  /// Poll for a single frame; emits 0 or 1 records.
  void poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;

  /// Stats accessor
  const ProducerStats& stats() const override { return stats_; }

  /// Whether device is opened
  bool is_open() const { return opened_; }

private:
  /**
   * @brief Open the device if not already opened
   *
   * @return true on successful open or already opened, false on failure
   */
  bool open_if_needed();

  /// @brief Configuration parameters
  Config cfg_;

  /// @brief Statistics
  ProducerStats stats_{};

  /// @brief Monotonic sequence number for emitted records
  uint64_t seq_ = 0;

  /// @brief Persistent capture handle (opened in start, released in stop)
  cv::VideoCapture cap_;

  /// @brief Whether we've opened the device
  bool opened_{false};
};

} // namespace trossen::hw::camera

#endif  // TROSSEN_SDK__HW__CAMERA__OPENCV_PRODUCER_HPP
