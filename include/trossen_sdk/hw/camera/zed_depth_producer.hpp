/**
 * @file zed_depth_producer.hpp
 * @brief Camera producer for depth using ZED SDK for real hardware.
 */

#ifndef TROSSEN_SDK__HW__CAMERA__ZED_DEPTH_PRODUCER_HPP
#define TROSSEN_SDK__HW__CAMERA__ZED_DEPTH_PRODUCER_HPP

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
 * @brief Depth camera producer using ZED SDK for real hardware.
 */
class ZedDepthCameraProducer : public ::trossen::hw::PolledProducer {
public:
  /**
   * @brief Configuration parameters for ZedDepthCameraProducer
   */
  struct Config {
    /// @brief Serial Number of the camera device
    int serial_number{0};

    /// @brief Logical stream identifier (e.g. "zed_depth_camera_main")
    std::string stream_id{"zed_depth_camera_main"};

    /// @brief Desired output image encoding (e.g. "16UC1" for 16-bit depth)
    std::string encoding{"16UC1"};

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
   * @brief Metadata specific to ZedDepthCameraProducer
   */
  struct ZedDepthCameraProducerMetadata : public PolledProducer::ProducerMetadata {
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
    bool is_depth_map{true};

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
      camera_feature["shape"] = {height, width, 1};
      camera_feature["names"] = {"height", "width", "channel"};
      camera_feature["info"] = {
        {"video.encoding", "depth"}
      };
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
   * @brief Construct ZedDepthCameraProducer
   *
   * @param frame_cache Shared frame cache from ZedCameraComponent
   * @param cfg Configuration parameters
   */
  ZedDepthCameraProducer(std::shared_ptr<ZedFrameCache> frame_cache, Config cfg);

  /**
   * @brief Destructor
   */
  ~ZedDepthCameraProducer() override;

  /**
   * @brief Poll for new depth frames
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
    return std::make_shared<ZedDepthCameraProducerMetadata>(metadata_);
  }

private:
  /// @brief Frame cache shared with color producer
  std::shared_ptr<ZedFrameCache> frame_cache_;

  /// @brief Configuration
  Config cfg_;

  /// @brief Metadata
  ZedDepthCameraProducerMetadata metadata_;

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

#endif  // TROSSEN_SDK__HW__CAMERA__ZED_DEPTH_PRODUCER_HPP
