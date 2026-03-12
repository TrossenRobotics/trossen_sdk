/**
 * @file realsense_push_producer.hpp
 * @brief Unified push-style producer for RealSense cameras (color + optional depth).
 */

#ifndef TROSSEN_SDK__HW__CAMERA__REALSENSE_PUSH_PRODUCER_HPP_
#define TROSSEN_SDK__HW__CAMERA__REALSENSE_PUSH_PRODUCER_HPP_

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "librealsense2/rs.hpp"
#include "nlohmann/json.hpp"
#include "opencv2/imgproc.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

namespace trossen::hw::camera {

/**
 * @brief Unified PushProducer for RealSense cameras.
 *
 * Owns a dedicated thread that calls wait_for_frames() on the rs2::pipeline.
 * Supports both color-only and color+depth capture modes. When depth alignment
 * is enabled, an rs2::align instance is created once and reused per frame.
 *
 * Registered as "realsense_camera" in PushProducerRegistry.
 */
class RealsensePushProducer : public ::trossen::hw::PushProducer {
public:
  /**
   * @brief Configuration parameters for RealsensePushProducer
   */
  struct Config {
    /// @brief Logical stream identifier used as MCAP topic / file path prefix
    std::string stream_id{"realsense_camera"};

    /// @brief Desired output color encoding ("bgr8" or "rgb8")
    std::string color_encoding{"bgr8"};

    /// @brief Prefer device hardware timestamp over host clock
    bool use_device_time{true};

    /// @brief Timeout ms for wait_for_frames()
    int timeout_ms{3000};

    /// @brief Target FPS (for FPS health reporting only; actual rate set by hardware)
    int fps{30};
  };

  /**
   * @brief Metadata for RealsensePushProducer
   */
  struct RealsensePushProducerMetadata : public ::trossen::hw::PolledProducer::ProducerMetadata {
    int width{0};
    int height{0};
    int fps{30};
    std::string codec{"av1"};
    std::string pix_fmt{"yuv420p"};
    int channels{3};
    bool has_audio{false};
    bool has_depth{false};

    nlohmann::ordered_json get_info() const override {
      nlohmann::ordered_json features;

      // Color stream feature
      nlohmann::ordered_json color_feature;
      color_feature["id"] = id;
      color_feature["dtype"] = "video";
      color_feature["shape"] = {height, width, channels};
      color_feature["names"] = {"height", "width", "channels"};
      color_feature["info"] = {
        {"video.fps", fps},
        {"video.height", height},
        {"video.width", width},
        {"video.channels", channels},
        {"video.codec", codec},
        {"video.pix_fmt", pix_fmt},
        {"video.is_depth_map", false},
        {"has_audio", has_audio}};
      features["observation.images." + id] = color_feature;

      // Depth stream feature (when present)
      if (has_depth) {
        nlohmann::ordered_json depth_feature;
        std::string depth_id = id + "_depth";
        depth_feature["id"] = depth_id;
        depth_feature["dtype"] = "video";
        depth_feature["shape"] = {height, width, 1};
        depth_feature["names"] = {"height", "width", "channels"};
        // ffv1: lossless codec required for metric depth data
        // gray16le: matches RealSense Z16 depth format (16-bit unsigned, little-endian)
        depth_feature["info"] = {
          {"video.fps", fps},
          {"video.height", height},
          {"video.width", width},
          {"video.channels", 1},
          {"video.codec", "ffv1"},
          {"video.pix_fmt", "gray16le"},
          {"video.is_depth_map", true},
          {"has_audio", false}};
        features["observation.images." + depth_id] = depth_feature;
      }

      return features;
    }

    nlohmann::ordered_json get_stream_info() const override {
      nlohmann::ordered_json info;
      info["cameras"][id] = {
        {"width", width},
        {"height", height},
        {"fps", fps},
        {"channels", channels},
        {"codec", codec},
        {"pix_fmt", pix_fmt},
        {"is_depth_map", false},
        {"has_audio", has_audio}};

      if (has_depth) {
        std::string depth_id = id + "_depth";
        info["cameras"][depth_id] = {
          {"width", width},
          {"height", height},
          {"fps", fps},
          {"channels", 1},
          {"codec", "ffv1"},
          {"pix_fmt", "gray16le"},
          {"is_depth_map", true},
          {"has_audio", false}};
      }

      return info;
    }
  };

  /**
   * @brief Construct a RealsensePushProducer
   *
   * @param hardware Hardware component (must be RealsenseCameraComponent)
   * @param config JSON configuration
   * @throws std::runtime_error if hardware is wrong type or pipeline is null
   */
  RealsensePushProducer(
    std::shared_ptr<::trossen::hw::HardwareComponent> hardware,
    const nlohmann::json& config);

  ~RealsensePushProducer() override;

  /**
   * @brief Start the push thread and register emit callback
   *
   * @param emit Callback invoked once per captured frameset
   * @return true on success
   */
  bool start(
    const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;

  /**
   * @brief Stop the push thread (blocks until thread exits)
   */
  void stop() override;

  /**
   * @brief Get producer metadata
   *
   * @return Shared pointer to RealsensePushProducerMetadata
   */
  std::shared_ptr<::trossen::hw::PolledProducer::ProducerMetadata> metadata() const override {
    return std::make_shared<RealsensePushProducerMetadata>(metadata_);
  }

private:
  /**
   * @brief Main push loop (runs on push_thread_)
   *
   * Calls wait_for_frames(), builds ImageRecord (with optional depth), emits.
   */
  void push_loop(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit);

  /// @brief Parsed configuration
  Config cfg_;

  /// @brief Producer metadata (populated in constructor)
  RealsensePushProducerMetadata metadata_;

  /// @brief Dedicated push thread
  std::thread push_thread_;

  /// @brief Running flag (atomic for thread-safe stop)
  std::atomic<bool> running_{false};

  /// @brief RealSense pipeline (shared from RealsenseCameraComponent)
  std::shared_ptr<rs2::pipeline> pipeline_;

  /// @brief depth scale factor (from RealsenseCameraComponent)
  float depth_scale_{0.0f};

  /// @brief Whether depth capture is enabled
  bool use_depth_{false};

  /// @brief Depth aligner (created once if use_depth_; reused per frame)
  std::unique_ptr<rs2::align> aligner_;
};

}  // namespace trossen::hw::camera

#endif  // TROSSEN_SDK__HW__CAMERA__REALSENSE_PUSH_PRODUCER_HPP_
