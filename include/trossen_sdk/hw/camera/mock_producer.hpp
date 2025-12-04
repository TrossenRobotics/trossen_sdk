/**
 * @file mock_producer.hpp
 * @brief Synthetic camera producer for testing without real hardware.
 */

#ifndef TROSSEN_SDK__HW__CAMERA__MOCK_PRODUCER_HPP
#define TROSSEN_SDK__HW__CAMERA__MOCK_PRODUCER_HPP

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

namespace trossen::hw::camera {

/**
 * @brief Mock camera that synthetically generates frames at (approximately) a target FPS.
 *
 * This is a polled producer; callers may poll at any cadence. The producer will only emit a frame
 * when enough time has elapsed since the last emitted frame to respect the configured target frame
 * period (unless fps==0, in which case it emits every poll).
 */
class MockCameraProducer : public ::trossen::hw::PolledProducer {
public:
  /**
   * @brief Pattern types for synthetic image generation
   */
  enum class Pattern {
    /// @brief Solid color pattern (fastest generation)
    Solid,

    /// @brief Gradient pattern
    Gradient,

    /// @brief Noise pattern
    Noise
  };

  /**
   * @brief Configuration parameters for MockCameraProducer
   */
  struct Config {
    /// @brief Image width in pixels
    int width{1920};

    /// @brief Image height in pixels
    int height{1080};

    /// @brief Target frames per second (0 = emit every poll)
    int fps{60};

    /// @brief Cache and reuse a single frame (huge performance boost)
    bool cache_frames{false};

    /// @brief Logical stream identifier
    std::string stream_id{"mock_cam"};

    /// @brief Image encoding (e.g. "bgr8", "rgb8", "mono8", etc)
    std::string encoding{"bgr8"};

    /// @brief Image pattern to generate
    Pattern pattern{Pattern::Gradient};

    /// @brief Random seed for noise pattern
    uint64_t seed{0};

    /// @brief Stddev for noise pattern
    double noise_stddev{20.0};

    /// @brief For MovingSquare pattern
    int square_size{120};

    /// @brief Frames to generate & discard before emitting
    int warmup_frames{0};

    /// @brief Simulated drop probability [0,1)
    double drop_probability{0.0};
  };

  /**
   * @brief Metadata specific to MockCameraProducer
   */
  struct MockCameraProducerMetadata : public PolledProducer::ProducerMetadata {
    /// @brief Image width
    int width;

    /// @brief Image height
    int height;

    // TODO(shantanuparab-tr): Can be made enum
    /// @brief Image encoding
    std::string codec;

    // TODO(shantanuparab-tr): Can be made enum
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
   * @brief Construct a MockCameraProducer
   *
   * @param cfg Configuration parameters
   */
  explicit MockCameraProducer(Config cfg);

  /**
   * @brief Destructor
   */
  ~MockCameraProducer() override = default;

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
    return std::make_shared<MockCameraProducerMetadata>(metadata_);
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
   * @brief Generate a synthetic frame into the provided Mat
   *
   * @param[out] dst Destination Mat to fill
   */
  void generate_frame(cv::Mat &dst);

  /// @brief Configuration parameters
  Config cfg_;

  /// @brief Target frame period in nanoseconds
  uint64_t frame_period_ns_{0};

  /// @brief Last emitted frame monotonic timestamp
  uint64_t last_emit_mono_{0};

  /// @brief Number of warmup frames remaining
  int warmup_remaining_{0};

  /// @brief PRNG state
  std::mt19937 rng_;

  /// @brief Normal distribution for noise pattern
  std::normal_distribution<double> noise_norm_{0.0, 1.0};

  /// @brief Uniform distribution for drop simulation
  std::uniform_real_distribution<double> drop_dist_{0.0, 1.0};

  /// @brief Recorded inter-frame intervals in nanoseconds
  std::vector<uint64_t> intervals_ns_;

  /// @brief Maximum stored intervals for jitter stats
  static constexpr size_t kMaxIntervals = 50'000;

  /// @brief Producer metadata
  MockCameraProducerMetadata metadata_;

  /// @brief Pre-allocated frame buffer (reused to avoid allocations)
  cv::Mat frame_buffer_;

  /// @brief Cached frame for reuse mode
  cv::Mat cached_frame_;

  /// @brief Whether cached frame has been generated
  bool cache_valid_{false};
};

}  // namespace trossen::hw::camera

#endif  // TROSSEN_SDK__HW__CAMERA__MOCK_PRODUCER_HPP
