/**
 * @file lerobot_backend.hpp
 * @brief LeRobot backend: writes joint states to Parquet and images to directory tree.
 * Converts images to videos per source using FFmpeg after recording.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_BACKEND_HPP
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_BACKEND_HPP

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <nlohmann/json.hpp>
#include <parquet/arrow/writer.h>
#include "trossen_sdk/io/backend.hpp"
#include "trossen_sdk/io/backends/lerobot/lerobot_constants.hpp"

namespace trossen::io::backends {

/**
 * @brief Backend producing a simple on-disk layout for datasets.
 *
 * Layout (root = uri provided to open()):
 *   root/
 *     <repo-id>/
 *         <dataset-name>/
 *             data/
 *                 <chunk-id>/
 *                     <episode-id>.<data-format>
 *             meta/
 *                 <metadata-files>
 *             images/
 *                 <chunk-id>/
 *                    <source-id>/
 *                       <episode-id>/
 *                           <image-id>.<image-format>
 *             videos/
 *                 <chunk-id>/
 *                     <source-id>/
 *                         <video-id>.<video-format>
 */
class LeRobotBackend : public io::Backend {
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
  struct Config : public io::Backend::Config {
    // Root output directory
    std::string output_dir;

    // Number of image encoding threads
    size_t encoder_threads{1};

    // 0 = unbounded
    size_t max_image_queue{0};

    // Policy when max_image_queue > 0 and image queue is full
    DropPolicy drop_policy{DropPolicy::DropNewest};

    // PNG compression level (0-9) (May not be requiredd)
    int png_compression_level{3};

    // Overwrite existing files
    bool overwrite_existing{false};

    // Option to encode videos after recording
    bool encode_videos{false};

    // Task name for organizing datasets
    std::string task_name{"default_task"};

    // Repository ID
    std::string repository_id{"default_repo"};

    // Dataset name
    std::string dataset_name{"default_dataset"};

    // Root path
    std::string root_path{get_default_root_path().string()};

    // Episode index (for organizing output)
    uint32_t episode_index{0};

  };

  struct Metadata {
    // TODO (shantanuparab-tr): Add meaningful metadata fields with the Metadata PR

    // Name of the robot
    std::string robot_name;

    // Name of the task
    std::string task_name;

    // Version information (LeRobot)
    std::string codebase_version;

    // Trossen SDK subversion
    std::string trossen_subversion;

    // Number of cameras
    int num_cameras;

    // Number of action features
    int num_action_features;

    // Number of observation features
    int num_observation_features;

    // Camera Width
    int camera_width;

    // Camera Height
    int camera_height;

    // Is depth camera
    bool is_depth_camera;

    // Frame rate
    int fps;

    // Camera names
    std::vector<std::string> camera_names;

    // Action feature names
    std::vector<std::string> action_feature_names;

    // Observation feature names
    std::vector<std::string> observation_feature_names;

  };

  /**
   * @brief Construct a LeRobotBackend
   *
   * @param cfg Configuration parameters
   */
  explicit LeRobotBackend(Config cfg, Metadata md);

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

  /**
   * @brief Add metadata to be written to info.json
   *
   * @param md Metadata to add
   */
  void addMetadata(const Metadata& md);

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

    /// @brief Accumulated queue wait time before encode (ns)
    uint64_t queue_wait_ns_acc{0};

    /// @brief Max queue wait time (ns)
    uint64_t queue_wait_ns_max{0};

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

    /// @brief Average queue wait time (ms)
    double avg_queue_wait_ms() const {
      if (written == 0) return 0.0;
      return (queue_wait_ns_acc / 1e6) / static_cast<double>(written);
    }

    /// @brief Estimated per-thread encode throughput (fps) = threads * written / total_encode_wall
    /// NOTE: This is approximate; actual parallel overlap may differ.
    double est_per_thread_fps(size_t threads) const {
      if (threads == 0 || encode_time_ns_acc == 0) return 0.0;
      double total_s = encode_time_ns_acc / 1e9; // sum of per-frame times across all threads
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
  // Parallel deque storing enqueue steady_clock timestamps for wait time measurement
  std::deque<std::chrono::steady_clock::time_point> image_queue_enqueue_times_;
  std::mutex image_queue_mutex_;
  std::condition_variable image_queue_cv_;
  // Encoder workers (multi-threaded encoding support)
  std::vector<std::thread> image_workers_;
  // Config for this backend
  Config cfg_;
  // Metadata for this backend
  Metadata md_;
  std::atomic<bool> image_worker_running_{false};

  // Basic stats
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

  // Derived / cached config values
  size_t max_image_queue_cached_{0};

  std::filesystem::path root_;
  std::filesystem::path images_root_;
  std::filesystem::path videos_root_;
  std::filesystem::path meta_root_;
  std::filesystem::path data_root_;
  std::mutex write_mutex_;
  std::mutex open_mutex_;
  bool opened_{false};
  std::unordered_map<std::string, std::filesystem::path> image_dir_cache_;


  std::shared_ptr<arrow::Schema> schema_;
  std::shared_ptr<arrow::io::FileOutputStream> outfile_;
  std::unique_ptr<parquet::arrow::FileWriter> writer_;
};

} // namespace trossen::io::backends

#endif // TROSSEN_SDK__IO__BACKENDS__TROSSEN_HPP
