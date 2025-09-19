/**
 * @file lerobot_v2.hpp
 * @brief LeRobot V2 backend: writes joint states to CSV and images to directory tree.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2_HPP
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2_HPP

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "trossen_sdk/io/backend.hpp"

namespace trossen::io::backends {

/**
 * @brief Backend producing a simple on-disk layout for LeRobot-style datasets.
 *
 * Layout (root = uri provided to open()):
 *   root/
 *     joint_states.csv
 *     observations/
 *       images/
 *         <camera_id>/
 *           <ts_monotonic_ns>.png
 */
class LeRobotV2Backend : public io::Backend {
public:
  /// @brief Image queue drop policy when full
  enum class DropPolicy {

    /// @brief Drop newest incoming image
    DropNewest,

    /// @brief Drop oldest image in queue to make room for new one
    DropOldest,

    /// @brief Block until space is available (not recommended)
    // Block
  };

  /// @brief Configuration parameters
  struct Config {
    // Root output directory
    std::string output_dir;

    // Number of image encoding threads
    size_t encoder_threads{1};

    // 0 = unbounded
    size_t max_image_queue{0};

    // Policy when max_image_queue > 0 and image queue is full
    DropPolicy drop_policy{DropPolicy::DropNewest};

    // PNG compression level (0-9)
    int png_compression_level{3};
  };

  /**
   * @brief Construct a LeRobotV2Backend
   *
   * @param cfg Configuration parameters
   */
  explicit LeRobotV2Backend(Config cfg);

  /**
   * @brief Open a LeRobot V2 logging destination
   *
   * @return true on success
   */
  bool open() override;

  /**
   * @brief Write a single record
   *
   * @param record Record to write
   */
  void write(const data::RecordBase& record) override;

  /**
   * @brief Write a batch of records
   *
   * @param records Records to write
   */
  void writeBatch(std::span<const data::RecordBase* const> records) override;

  /**
   * @brief Flush any buffered data
   */
  void flush() override;

  /**
   * @brief Close the backend
   */
  void close() override;

  /// @brief Image encoding statistics
  struct ImageEncodeStats {

    /// @brief Total images enqueued
    uint64_t enqueued{0};

    /// @brief Total images successfully encoded & written
    uint64_t written{0};

    /// @brief Total images dropped due to queue full
    uint64_t dropped{0};

    /// @brief Accumulated image encode time (ns)
    uint64_t encode_time_ns_acc{0};

    /// @brief Maximum single image encode time (ns)
    uint64_t encode_time_ns_max{0};

    /// @brief High water mark of image queue length
    size_t queue_high_water{0};

    /// @brief Average image queue length (samples taken at enqueue time)
    double avg_backlog{0.0};

    /// @brief Average image encode time (ms)
    double avg_encode_ms() const {
      // Avoid divide-by-zero
      if (written == 0) {
        return 0.0;
      }
      return (encode_time_ns_acc / 1e6) / static_cast<double>(written);
    }
  };

  /**
   * @brief Access image encoding stats
   *
   * @return Image encoding statistics
   */
  ImageEncodeStats image_encode_stats() const {
    ImageEncodeStats s;
    s.enqueued = img_enqueued_.load(std::memory_order_relaxed);
    s.written = img_encoded_.load(std::memory_order_relaxed);
    s.dropped = img_dropped_.load(std::memory_order_relaxed);
    s.encode_time_ns_acc = img_encode_time_ns_acc_.load(std::memory_order_relaxed);
    s.encode_time_ns_max = img_encode_time_ns_max_.load(std::memory_order_relaxed);
    s.queue_high_water = img_queue_high_water_.load(std::memory_order_relaxed);
    uint64_t backlog_samples = img_queue_backlog_samples_.load(std::memory_order_relaxed);
    uint64_t backlog_sum = img_queue_backlog_sum_.load(std::memory_order_relaxed);
    if (backlog_samples > 0) {
      s.avg_backlog = static_cast<double>(backlog_sum) / static_cast<double>(backlog_samples);
    }
    return s;
  }

  /**
   * @brief Access current config
   *
   * @return Current configuration
   */
  const Config& config() const { return cfg_; }

private:
  /**
   * @brief Write a joint state record to disk
   *
   * @param base Record to write (will be dynamic_cast to JointStateRecord)
   */
  void writeJointState(const data::RecordBase& base);

  /**
   * @brief Write an image record to disk
   *
   * @param base Record to write (will be dynamic_cast to ImageRecord)
   */
  void writeImage(const data::RecordBase& base);

  /**
   * @brief Write images to disk in a worker thread
   */
  void imageWorkerLoop();

  /// @brief Image encoding job
  struct ImageJob {
    /// @brief Full file path to write
    std::filesystem::path file_path;

    /// @brief Image to write
    cv::Mat image;
  };

  // Async image encoding members
  std::deque<ImageJob> image_queue_;
  std::mutex image_queue_mutex_;
  std::condition_variable image_queue_cv_;
  // Encoder workers (multi-threaded encoding support)
  std::vector<std::thread> image_workers_;
  // Config for this backend
  Config cfg_;
  std::atomic<bool> image_worker_running_{false};

  // Basic stats
  std::atomic<uint64_t> img_enqueued_{0};
  std::atomic<uint64_t> img_encoded_{0};
  std::atomic<uint64_t> img_dropped_{0};
  std::atomic<uint64_t> img_encode_time_ns_acc_{0};
  std::atomic<uint64_t> img_encode_time_ns_max_{0};
  std::atomic<size_t> img_queue_high_water_{0};
  std::atomic<uint64_t> img_queue_backlog_sum_{0};
  std::atomic<uint64_t> img_queue_backlog_samples_{0};

  // Derived / cached config values
  size_t max_image_queue_cached_{0};

  std::filesystem::path root_;
  std::filesystem::path images_root_;
  std::ofstream joint_csv_;
  bool header_written_{false};
  std::mutex write_mutex_;
  std::mutex open_mutex_;
  bool opened_{false};
  std::unordered_map<std::string, std::filesystem::path> image_dir_cache_;
};

} // namespace trossen::io::backends

#endif // TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2_HPP
