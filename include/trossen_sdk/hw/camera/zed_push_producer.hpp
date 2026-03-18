/**
 * @file zed_push_producer.hpp
 * @brief Push-style producer for StereoLabs ZED stereo cameras (color + optional depth)
 *
 * Owns a dedicated grab thread that calls sl::Camera::grab() in a tight loop.
 * Each grabbed frame is converted to a cv::Mat and emitted as an ImageRecord
 * (with optional depth).
 *
 * Build gate: compiled only when TROSSEN_ENABLE_ZED=ON (CMake option).
 */

#ifndef TROSSEN_SDK__HW__CAMERA__ZED_PUSH_PRODUCER_HPP_
#define TROSSEN_SDK__HW__CAMERA__ZED_PUSH_PRODUCER_HPP_

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "nlohmann/json.hpp"
#include "opencv2/imgproc.hpp"
#include "sl/Camera.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

namespace trossen::hw::camera {

/**
 * @brief Unified PushProducer for ZED stereo cameras
 *
 * The producer uses VIEW::LEFT_BGR (SDK 5.x) to retrieve a 3-channel BGR
 * image directly from the GPU pipeline, avoiding a redundant BGRA->BGR
 * strip.  When depth is enabled, MEASURE::DEPTH (F32_C1) is retrieved and
 * sanitized (NaN/Inf replaced, then quantized to CV_16UC1 millimetres)
 * for downstream compatibility with the RealSense depth path.
 *
 * Registered as "zed_camera" in PushProducerRegistry.
 */
class ZedPushProducer : public ::trossen::hw::PushProducer {
public:
  /**
   * @brief Configuration parameters for ZedPushProducer
   */
  struct Config {
    std::string stream_id{"zed_camera"};
    std::string color_encoding{"bgr8"};
    bool use_device_time{true};
    int timeout_ms{3000};
    int fps{30};
    bool remove_saturated_areas{false};  ///< SDK 5.x default is false
  };

  /**
   * @brief Metadata for ZedPushProducer (mirrors RealsensePushProducerMetadata)
   */
  struct ZedPushProducerMetadata : public ::trossen::hw::PolledProducer::ProducerMetadata {
    int width{0};
    int height{0};
    int fps{30};
    std::string codec{"av1"};
    std::string pix_fmt{"yuv420p"};
    int channels{3};
    bool has_audio{false};
    bool has_depth{false};

    nlohmann::ordered_json get_info() const override;
    nlohmann::ordered_json get_stream_info() const override;
  };

  /**
   * @brief Construct a ZedPushProducer
   *
   * @param hardware Hardware component (must be ZedCameraComponent)
   * @param config JSON configuration
   * @throws std::runtime_error if hardware is wrong type or camera is null
   */
  ZedPushProducer(
    std::shared_ptr<::trossen::hw::HardwareComponent> hardware,
    const nlohmann::json& config);

  ~ZedPushProducer() override;

  bool start(
    const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;
  void stop() override;

  std::shared_ptr<::trossen::hw::PolledProducer::ProducerMetadata> metadata() const override {
    return std::make_shared<ZedPushProducerMetadata>(metadata_);
  }

private:
  /// @brief Main grab + emit loop (runs on push_thread_)
  void push_loop(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit);

  Config cfg_;
  ZedPushProducerMetadata metadata_;
  std::thread push_thread_;
  std::atomic<bool> running_{false};

  /// @brief Shared ZED camera handle (from ZedCameraComponent)
  std::shared_ptr<sl::Camera> camera_;

  /// @brief Whether depth capture is enabled
  bool use_depth_{false};

  /// @brief Reusable sl::Mat buffers to avoid per-frame GPU allocation
  sl::Mat sl_color_;
  sl::Mat sl_depth_;
};

}  // namespace trossen::hw::camera

#endif  // TROSSEN_SDK__HW__CAMERA__ZED_PUSH_PRODUCER_HPP_
