/**
 * @file realsense_depth_producer.hpp
 * @brief Camera producer for depth using Realsense SDK for real hardware.
 */

#ifndef TROSSEN_SDK__HW__CAMERA__REALSENSE_DEPTH_PRODUCER_HPP
#define TROSSEN_SDK__HW__CAMERA__REALSENSE_DEPTH_PRODUCER_HPP
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>


#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/camera/realsense_frame_cache.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

namespace trossen::hw::camera {

/**
 * @brief Camera producer using Realsense SDK for real hardware.
 */
class RealsenseDepthCameraProducer : public ::trossen::hw::PolledProducer {
public:
  /**
   * @brief Configuration parameters for RealsenseDepthCameraProducer
   */
  struct Config {
    /// @brief Serial Number of the camera device (012345678, etc.)
    std::string serial_number{"012345678"};

    /// @brief Logical stream identifier (e.g. "realsense_camera0")
    std::string stream_id{"realsense_camera0"};

    /// @brief Desired output image encoding (e.g. "bgr8", "rgb8", "mono8")
    std::string encoding{"bgr8"};

    /// @brief Desired image width (pixels). 0 = leave default
    int width{0};

    /// @brief Desired image height (pixels). 0 = leave default
    int height{0};

    /// @brief Desired frame rate (fps). 0 = leave default
    int fps{0};

    /// @brief Prefer device timestamp if available
    bool use_device_time{true};

    /// @brief Seconds to warm up (discard frames) after device open before emitting
    double warmup_seconds{0.0};

    /// @brief Whether to enforce the requested fps (may reduce if device cannot keep up)
    bool enforce_requested_fps = true;

    /// @brief Flag to use depth map stream along with color stream
    bool use_depth_map{false};
  };

  /**
   * @brief Metadata specific to RealsenseDepthCameraProducer
   */
  struct RealsenseDepthCameraProducerMetadata : public PolledProducer::ProducerMetadata {
    /// @brief Image width
    int width;

    /// @brief Image height
    int height;

    // TODO(shantanuparab-tr): Can be made enum
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
      camera_feature["shape"] = {height, width, 3};
      camera_feature["names"] = {"height", "width", "channels"};
      camera_feature["info"] = {
          {"video.fps", fps},
          {"video.height", height},
          {"video.width", width},
          {"video.channels", channels},
          {"video.codec", codec},
          {"video.pix_fmt", pix_fmt},
          {"video.is_depth_map", is_depth_map},
          {"has_audio", has_audio}};
      features["observation.images." + id] = camera_feature;
      return features;
    }
  };

  /**
   * @brief Construct an RealsenseDepthCameraProducer
   *
   * @param camera Shared pointer to an rs2::pipeline object
   * @param cfg Configuration parameters
   */
  explicit RealsenseDepthCameraProducer(std::shared_ptr<RealsenseFrameCache> cache, Config cfg);

  /**
   * @brief Destructor
   */
  ~RealsenseDepthCameraProducer() override;

  /**
   * @brief Perform a blocking warmup (open device if needed, discard frames for warmup_seconds)
   *
   * @return true on success
   */
  bool warmup();

  /**
   * @brief Poll the producer for new data and emit records via the callback
   *
   * @param emit Callback to invoke for each produced record
   */
  void poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;

  /**
   * @brief Get producer metadata
   *
   * @return const reference to ProducerMetadata
   */
  std::shared_ptr<ProducerMetadata> metadata() const override {
    return std::make_shared<RealsenseDepthCameraProducerMetadata>(metadata_);
  }

protected:
  /**
   * @brief Open the device if not already opened
   *
   * @return true on successful open or already opened, false on failure
   */
  bool open_if_needed();

  /// @brief Configuration parameters
  Config cfg_;

  /// @brief Realsense cache of framesets
  std::shared_ptr<RealsenseFrameCache> frame_cache_;

  int64_t number_of_frames_captured_{0};


private:
  /// @brief Producer metadata
  RealsenseDepthCameraProducerMetadata metadata_;
};

}  // namespace trossen::hw::camera

#endif  // TROSSEN_SDK__HW__CAMERA__REALSENSE_DEPTH_PRODUCER_HPP
