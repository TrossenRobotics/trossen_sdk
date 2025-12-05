/**
 * @file trossen.hpp
 * @brief Trossen backend: writes joint states to CSV and images to directory tree.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__TROSSEN_BACKEND_HPP
#define TROSSEN_SDK__IO__BACKENDS__TROSSEN_BACKEND_HPP

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "trossen_sdk/io/backend.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/configuration/types/backends/trossen_backend_config.hpp"
#include "trossen_sdk/configuration/global_config.hpp"


namespace trossen::io::backends {

/**
 * @brief Backend producing a simple on-disk layout for datasets.
 *
 * Layout (root = uri provided to open()):
 *   root/
 *     joint_states.csv
 *     observations/
 *       images/
 *         <camera_id>/
 *           <ts_monotonic_ns>.png
 */
class TrossenBackend : public io::Backend {
public:

  /**
   * @brief Construct a TrossenBackend
   *
   * @param metadata Optional producer metadata
   */
  explicit TrossenBackend(
    const ProducerMetadataList& metadata = {});

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
  void write_batch(std::span<const data::RecordBase* const> records) override;

  /**
   * @brief Flush any buffered data
   */
  void flush() override;

  /**
   * @brief Close the backend
   */
  void close() override;

  /**
   * @brief Image encoding statistics
   */
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

    /// @brief Accumulated queue wait time before encode (ns)
    uint64_t queue_wait_ns_acc{0};

    /// @brief Max queue wait time (ns)
    uint64_t queue_wait_ns_max{0};

    /// @brief High water mark of image queue length
    size_t queue_high_water{0};

    /// @brief Average image queue length (samples taken at enqueue time)
    double avg_backlog{0.0};

    /**
     * @brief Average image encode time (ms)
     *
     * @return Average encode time in milliseconds
     */
    double avg_encode_ms() const {
      // Avoid divide-by-zero
      if (written == 0) {
        return 0.0;
      }
      return (encode_time_ns_acc / 1e6) / static_cast<double>(written);
    }

    /**
     * @brief Average queue wait time in milliseconds
     *
     * @return Average queue wait time in milliseconds
     */
    double avg_queue_wait_ms() const {
      if (written == 0) return 0.0;
      return (queue_wait_ns_acc / 1e6) / static_cast<double>(written);
    }

    /**
     * @brief Estimated per-thread encode throughput (fps)
     *
     * @param threads Number of encoding threads
     * @return Estimated per-thread encode throughput in frames per second
     * @note This is approximate; actual parallel overlap may differ.
     */
    double est_per_thread_fps(size_t threads) const {
      if (threads == 0 || encode_time_ns_acc == 0) return 0.0;
      // sum of per-frame times across all threads
      double total_s = encode_time_ns_acc / 1e9;
      double frames = static_cast<double>(written);
      // Each frame's encode time counted once; so average frame time = total_s / frames.
      double avg_frame_s = total_s / frames;
      if (avg_frame_s <= 0) return 0.0;
      double single_thread_capacity = 1.0 / avg_frame_s;
      return single_thread_capacity;
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
    s.queue_wait_ns_acc = img_queue_wait_time_ns_acc_.load(std::memory_order_relaxed);
    s.queue_wait_ns_max = img_queue_wait_time_ns_max_.load(std::memory_order_relaxed);
    s.queue_high_water = img_queue_high_water_.load(std::memory_order_relaxed);
    uint64_t backlog_samples = img_queue_backlog_samples_.load(std::memory_order_relaxed);
    uint64_t backlog_sum = img_queue_backlog_sum_.load(std::memory_order_relaxed);
    if (backlog_samples > 0) {
      s.avg_backlog = static_cast<double>(backlog_sum) / static_cast<double>(backlog_samples);
    }
    return s;
  }

private:
  /**
   * @brief Write a joint state record to disk
   *
   * @param base Record to write (will be dynamic_cast to JointStateRecord)
   */
  void write_joint_state(const data::RecordBase& base);

  /**
   * @brief Write an image record to disk
   *
   * @param base Record to write (will be dynamic_cast to ImageRecord)
   */
  void write_image(const data::RecordBase& base);

  /**
   * @brief Write images to disk in a worker thread
   */
  void image_worker_loop();

  /**
   * @brief Image encoding job
   */
  struct ImageJob {
    /// @brief Full file path to write
    std::filesystem::path file_path;

    /// @brief Image to write
    cv::Mat image;
  };

  /// @brief Async image encoding members
  std::deque<ImageJob> image_queue_;

  /// @brief Parallel deque storing enqueue steady_clock timestamps for wait time measurement
  std::deque<std::chrono::steady_clock::time_point> image_queue_enqueue_times_;

  /// @brief Mutex protecting image queue
  std::mutex image_queue_mutex_;

  /// @brief Condition variable for image queue
  std::condition_variable image_queue_cv_;

  /// @brief Encoder workers (multi-threaded encoding support)
  std::vector<std::thread> image_workers_;

  /// @brief Config for this backend
  std::shared_ptr<TrossenBackendConfig> cfg_;

  /// @brief Whether image worker threads should keep running
  std::atomic<bool> image_worker_running_{false};

  /// @brief Basic stats
  std::atomic<uint64_t> img_enqueued_{0};
  std::atomic<uint64_t> img_encoded_{0};
  std::atomic<uint64_t> img_dropped_{0};
  std::atomic<uint64_t> img_encode_time_ns_acc_{0};
  std::atomic<uint64_t> img_encode_time_ns_max_{0};
  std::atomic<uint64_t> img_queue_wait_time_ns_acc_{0};
  std::atomic<uint64_t> img_queue_wait_time_ns_max_{0};
  std::atomic<size_t> img_queue_high_water_{0};
  std::atomic<uint64_t> img_queue_backlog_sum_{0};
  std::atomic<uint64_t> img_queue_backlog_samples_{0};

  /// @brief Derived / cached config values
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

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__TROSSEN_HPP
