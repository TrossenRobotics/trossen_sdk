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
#include "trossen_sdk/hw/camera/camera_streams_config.hpp"
#include "trossen_sdk/hw/camera/camera_types.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

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
  /**
   * @brief Pattern types for synthetic image generation
   */
  enum class Pattern {
    /// @brief Gradient pattern
    Gradient,

    /// @brief Noise pattern
    Noise
  };

  /**
   * @brief Configuration parameters for MockSyncedCameraProducer
   */
  struct Config {
    /// @brief Logical stream identifier
    std::string stream_id{"camera0"};

    /// @brief Camera streams configuration
    CameraStreamsConfig streams;

    /// @brief Pattern type for synthetic image generation
    Pattern pattern{Pattern::Gradient};

    /// @brief Random seed for noise pattern
    uint64_t seed{0};

    /// @brief Stddev for noise pattern
    double noise_stddev{20.0};
  };

  /**
   * @brief Metadata specific to MockSyncedCameraProducer
   */
  struct MockSyncedCameraProducerMetadata : public PolledProducer::ProducerMetadata {
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
   * @brief Construct a MockSyncedCameraProducer
   */
  explicit MockSyncedCameraProducer(Config cfg);

  /**
   * @brief Destructor
   */
  ~MockSyncedCameraProducer() override = default;

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
    return std::make_shared<MockSyncedCameraProducerMetadata>(metadata_);
  }

  /**
   * @brief Jitter statistics for emitted frames
   */
  struct JitterStats {
    /// @brief Mean inter-frame interval in milliseconds
    double mean_ms{0};

    /// @brief 50th percentile inter-frame interval in milliseconds
    double p50_ms{0};

    /// @brief 95th percentile inter-frame interval in milliseconds
    double p95_ms{0};

    /// @brief 99th percentile inter-frame interval in milliseconds
    double p99_ms{0};

    /// @brief Maximum inter-frame interval in milliseconds
    double max_ms{0};

    /// @brief Number of samples
    size_t samples{0};
  };

  /**
   * @brief Get jitter statistics for emitted frames
   *
   * @return JitterStats struct
   */
  JitterStats jitter_stats() const;

private:
  /**
   * @brief Generate a synthetic color frame into the provided Mat
   *
   * @param[out] dst Destination Mat to fill
   * @param seq_counter Sequence counter for frame generation
   */
  void generate_color(cv::Mat &dst, uint64_t seq_counter);

  /**
   * @brief Generate a synthetic u16 depth frame into the provided Mat
   *
   * @param[out] dst Destination Mat to fill
   * @param seq_counter Sequence counter for frame generation
   */
  void generate_depth_u16(cv::Mat &dst, uint64_t seq_counter);

  /**
   * @brief Generate a synthetic f32 depth frame into the provided Mat
   *
   * @param[out] dst Destination Mat to fill
   * @param seq_counter Sequence counter for frame generation
   */
  void generate_depth_f32(cv::Mat &dst, uint64_t seq_counter);

  /// @brief Last emitted frame monotonic timestamp
  uint64_t last_emit_mono_{0};

  /// @brief Monotonic sequence number for emitted records
  uint64_t seq_{0};

  /// @brief Number of warmup frames remaining
  int warmup_remaining_{0};

  /// @brief Recorded inter-frame intervals in nanoseconds
  std::vector<uint64_t> intervals_ns_;

  /// @brief Maximum stored intervals for jitter stats
  static constexpr size_t kMaxIntervals = 50'000;

  /// @brief PRNG state
  std::mt19937 rng_;

  /// @brief Normal distribution for noise pattern
  std::normal_distribution<double> noise_norm_{0.0, 1.0};

  /// @brief Uniform distribution for drop simulation
  std::uniform_real_distribution<double> drop_dist_{0.0, 1.0};

  /// @brief Configuration parameters
  Config cfg_;

  /// @brief Producer metadata
  MockSyncedCameraProducerMetadata metadata_;
};

}  // namespace trossen::hw::camera

#endif  // TROSSEN_SDK__HW__CAMERA__MOCK_SYNCED_PRODUCER_HPP
