/**
 * @file zed_producer.hpp
 * @brief Camera producer using ZED SDK for real hardware.
 */

#ifndef TROSSEN_SDK__HW__CAMERA__ZED_PRODUCER_HPP
#define TROSSEN_SDK__HW__CAMERA__ZED_PRODUCER_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "opencv2/imgproc.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/camera/zed_frame_cache.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

namespace trossen::hw::camera {

/**
 * @brief Camera producer using ZED SDK for real hardware.
 */
class ZedCameraProducer : public ::trossen::hw::PolledProducer {
public:
  /**
   * @brief Configuration parameters for ZedCameraProducer
   */
  struct Config {
    /// @brief Serial Number of the camera device
    int serial_number{0};

    /// @brief Logical stream identifier (e.g. "zed_camera_main")
    std::string stream_id{"zed_camera_main"};

    /// @brief Desired output image encoding (e.g. "bgr8", "rgb8", "bgra8")
    std::string encoding{"bgr8"};

    /// @brief Desired image width (pixels)
    int width{0};

    /// @brief Desired image height (pixels)
    int height{0};

    /// @brief Desired frame rate (fps)
    int fps{30};

    /// @brief Prefer device timestamp if available
    bool use_device_time{true};

    /// @brief Whether to enforce the requested fps (may reduce if device cannot keep up)
    bool enforce_requested_fps = true;
  };

  /**
   * @brief Metadata specific to ZedCameraProducer
   */
  struct ZedCameraProducerMetadata : public PolledProducer::ProducerMetadata {
    /// @brief Image width
    int width;

    /// @brief Image height
    int height;

    /// @brief Image encoding
    std::string codec;

    /// @brief Pixel format
    std::string pix_fmt;

    /// @brief Channels
    int channels;

    /// @brief Does the camera have an audio stream?
    bool has_audio{false};

    /// @brief Target frames per second
    int fps;

    /// @brief Is this a depth camera?
    bool is_depth_map{false};

    /**
     * @brief Get producer info as JSON
     *
     * @return JSON object containing producer information
     */
    nlohmann::ordered_json get_info() const override {
      nlohmann::ordered_json features;
      nlohmann::ordered_json camera_feature;
      camera_feature["id"] = id;
      camera_feature["dtype"] = "video";
      camera_feature["shape"] = {height, width, 3};
      camera_feature["names"] = {"height", "width", "channel"};
      features["observation.images." + name] = camera_feature;

      nlohmann::ordered_json j = PolledProducer::ProducerMetadata::get_info();
      j["features"] = features;
      j["width"] = width;
      j["height"] = height;
      j["codec"] = codec;
      j["pix_fmt"] = pix_fmt;
      j["channels"] = channels;
      j["has_audio"] = has_audio;
      j["fps"] = fps;
      j["is_depth_map"] = is_depth_map;

      return j;
    }
  };

  /**
   * @brief Construct ZedCameraProducer
   *
   * @param frame_cache Shared frame cache from ZedCameraComponent
   * @param cfg Configuration parameters
   */
  ZedCameraProducer(std::shared_ptr<ZedFrameCache> frame_cache, Config cfg);

  /**
   * @brief Destructor
   */
  ~ZedCameraProducer() override;

  /**
   * @brief Poll for new image frames
   *
   * @param emit Callback to emit records
   */
  void poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;

  /**
   * @brief Get producer metadata
   *
   * @return Shared pointer to metadata
   */
  std::shared_ptr<ProducerMetadata> metadata() const override {
    return std::make_shared<ZedCameraProducerMetadata>(metadata_);
  }

private:
  /// @brief Frame cache shared with other producers
  std::shared_ptr<ZedFrameCache> frame_cache_;

  /// @brief Configuration
  Config cfg_;

  /// @brief Metadata
  ZedCameraProducerMetadata metadata_;

  /// @brief Sequence number for frames
  uint64_t seq_{0};

  /// @brief Whether camera has been opened
  bool opened_{false};

  /// @brief Last capture monotonic timestamp (ns) for inter-frame timing
  uint64_t last_capture_mono_{0};

  /// @brief Inter-frame timing accumulator (ns)
  uint64_t if_accum_ns_{0};

  /// @brief Max inter-frame time (ns)
  uint64_t if_max_ns_{0};

  /// @brief Number of inter-frame samples
  uint64_t if_samples_{0};

  /// @brief Next frame number to report health stats
  uint64_t next_health_report_frame_{300};
};

}  // namespace trossen::hw::camera

#endif  // TROSSEN_SDK__HW__CAMERA__ZED_PRODUCER_HPP
