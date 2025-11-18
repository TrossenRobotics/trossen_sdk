/**
 * @file mock_synced_producer.hpp
 * @brief Synthetic paired (color + optional depth) camera producer emitting synchronized frames.
 */
#ifndef TROSSEN_SDK__HW__CAMERA__MOCK_SYNCED_PRODUCER_HPP
#define TROSSEN_SDK__HW__CAMERA__MOCK_SYNCED_PRODUCER_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "opencv2/core.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/hw/camera/camera_streams_config.hpp"
#include "trossen_sdk/hw/camera/camera_types.hpp"

namespace trossen::hw::camera {

/**
 * @brief MockSyncedCameraProducer generates synchronized color + depth frames sharing timestamps & seq.
 *
 * Depth emission enabled when cfg.capability == ColorAndDepth (or cfg.enable_depth set before normalize()).
 * Record IDs are fully-qualified topic-like paths:
 *   /cameras/<name>/color/image
 *   /cameras/<name>/depth/image
 */
class MockSyncedCameraProducer : public ::trossen::hw::PolledProducer {
public:
  enum class Pattern { Gradient, Noise }; // Reuse simple patterns for color

  struct Config {
    std::string camera_name{"camera0"};
    CameraStreamsConfig streams; // Contains frame_rate, resolutions, depth format, etc.
    Pattern pattern{Pattern::Gradient};
    uint64_t seed{0};
    double noise_stddev{20.0};
  };

  struct MockSyncedCameraProducerMetadata : public PolledProducer::ProducerMetadata {

    /// @brief Camera name
    std::string camera_name;

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

  };

  explicit MockSyncedCameraProducer(Config cfg);
  ~MockSyncedCameraProducer() override = default;

  void poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;

  /// @brief Get producer metadata
  std::shared_ptr<ProducerMetadata> metadata() const override {
    return std::make_shared<MockSyncedCameraProducerMetadata>(metadata_);
  }
  struct JitterStats {
    double mean_ms{0};
    double p50_ms{0};
    double p95_ms{0};
    double p99_ms{0};
    double max_ms{0};
    size_t samples{0};
  };
  JitterStats jitter_stats() const;

private:
  void generate_color(cv::Mat &dst, uint64_t seq_counter);
  void generate_depth_u16(cv::Mat &dst, uint64_t seq_counter);  // CV_16UC1
  void generate_depth_f32(cv::Mat &dst, uint64_t seq_counter);  // CV_32FC1

  uint64_t frame_period_ns_{0};
  uint64_t last_emit_mono_{0};
  uint64_t seq_{0};
  int warmup_remaining_{0};

  // jitter intervals
  std::vector<uint64_t> intervals_ns_;
  static constexpr size_t kMaxIntervals = 50'000;

  // PRNG for noise pattern
  std::mt19937 rng_;
  std::normal_distribution<float> norm_{0.f, 1.f};

  Config cfg_;

  MockSyncedCameraProducerMetadata metadata_;
};

} // namespace trossen::hw::camera

#endif // TROSSEN_SDK__HW__CAMERA__MOCK_SYNCED_PRODUCER_HPP
