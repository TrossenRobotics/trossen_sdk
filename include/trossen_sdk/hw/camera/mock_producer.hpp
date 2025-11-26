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
 * This is a polled producer; callers may poll at any cadence. The producer will only
 * emit a frame when enough time has elapsed since the last emitted frame to respect
 * the configured target frame period (unless fps==0, in which case it emits every poll).
 */
class MockCameraProducer : public ::trossen::hw::PolledProducer {
public:
  enum class Pattern {
    Gradient,
    Noise
  };

  struct Config {
    int width{1920};
    int height{1080};
    int fps{60};               ///< Target frames per second (0 = emit every poll)
    std::string stream_id{"mock_cam"};
    std::string encoding{"bgr8"};
    Pattern pattern{Pattern::Gradient};
    uint64_t seed{0};          ///< Deterministic seed (0 = pick fixed default)
    double noise_stddev{20.0}; ///< For Noise pattern (0-255 scale)
    int square_size{120};      ///< For MovingSquare pattern
    int warmup_frames{0};      ///< Frames to generate & discard before emitting
    double drop_probability{0.0}; ///< Simulated drop probability [0,1)
  };

  struct MockCameraProducerMetadata : public PolledProducer::ProducerMetadata {

    /// @brief Image width
    int width;

    /// @brief Image height
    int height;

    // TODO (shantanuparab-tr): Can be made enum
    /// @brief Image encoding
    std::string codec;

    // TODO (shantanuparab-tr): Can be made enum
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

    /// @brief Get producer info as JSON
    /// @return JSON object containing producer information
    nlohmann::ordered_json get_info() const override {
      nlohmann::ordered_json features;
      nlohmann::ordered_json camera_feature;
      camera_feature["id"] = id;
      camera_feature["dtype"] = "video";
      camera_feature["shape"] = {height, width, 3};
      camera_feature["names"] = {"height", "width", "channels"};
      camera_feature["info"] = {
          {"video.fps", fps},           {"video.height", height},
          {"video.width", width},       {"video.channels", channels},
          {"video.codec", codec},                {"video.pix_fmt", pix_fmt},
          {"video.is_depth_map", is_depth_map}, {"has_audio", has_audio}};
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

  ~MockCameraProducer() override = default;

  void poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;

  /// @brief Get producer metadata
  std::shared_ptr<ProducerMetadata> metadata() const override {
    return std::make_shared<MockCameraProducerMetadata>(metadata_);
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
  void generate_frame(cv::Mat &dst);

  Config cfg_;
  uint64_t frame_period_ns_{0};
  uint64_t last_emit_mono_{0};
  int warmup_remaining_{0};

  // PRNG state
  std::mt19937 rng_;
  std::normal_distribution<double> noise_norm_{0.0, 1.0};
  std::uniform_real_distribution<double> drop_dist_{0.0, 1.0};

  // Jitter measurement: store recent inter-frame intervals (ns)
  std::vector<uint64_t> intervals_ns_;
  static constexpr size_t kMaxIntervals = 50000; // cap to avoid unbounded growth

  MockCameraProducerMetadata metadata_;
};

} // namespace trossen::hw::camera

#endif // TROSSEN_SDK__HW__CAMERA__MOCK_PRODUCER_HPP
