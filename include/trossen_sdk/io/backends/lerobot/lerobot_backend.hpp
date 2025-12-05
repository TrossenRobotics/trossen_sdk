/**
 * @file lerobot_backend.hpp
 * @brief LeRobot backend: writes joint states to Parquet and images to directory tree. Converts
 * images to videos per source using FFmpeg after recording.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_BACKEND_HPP
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_BACKEND_HPP

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <nlohmann/json.hpp>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/io/backend.hpp"
#include "trossen_sdk/io/backends/lerobot/lerobot_constants.hpp"
#include "trossen_sdk/configuration/types/backends/lerobot_backend_config.hpp"

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
  /**
   * @brief Construct a LeRobotBackend
   *
   * @param metadata Vector of producer metadata to include in info.json
   */
  explicit LeRobotBackend(
    ProducerMetadataList metadata);

  /**
   * @brief Prepare backend for a new episode
   *
   * @param output_path Base output path for this episode
   * @param episode_index Zero-based episode index
   * @param dataset_id Dataset identifier
   * @param repository_id Repository identifier (unused)
   */
  void preprocess_episode(
    const std::string& output_path,
    uint32_t episode_index,
    const std::string& dataset_id,
    const std::string& repository_id) override;

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
   * @brief Add metadata to be written to info.json
   *
   * @param md Metadata to add
   */
  void write_metadata();

  /**
   * @brief Convert recorded images to videos using FFmpeg
   */
  void convert_to_videos() const;

  /**
   * @brief Compute and print statistics about the recorded data
   */
  void compute_statistics() const;

  /**
   * @brief Print statistics in a tabular format
   *
   * @param stats JSON object containing the statistics
   */
  void print_stats_table(const nlohmann::ordered_json& stats) const;

  /**
   * @brief Compute statistics for a ListArray
   *
   * @param list_array Shared pointer to the ListArray
   * @return JSON object containing the computed statistics
   */
  nlohmann::ordered_json compute_list_stats(
    const std::shared_ptr<arrow::ListArray> &list_array) const;

  /**
   * @brief Compute statistics for a flat array
   *
   * @param array Shared pointer to the flat array
   * @return JSON object containing the computed statistics
   */
  nlohmann::ordered_json compute_flat_stats(const std::shared_ptr<arrow::Array> &array) const;

  /**
   * @brief Compute statistics for a set of images
   *
   * @param images Vector of OpenCV Mat objects representing the images
   * @return FeatureStats object containing the computed statistics
   */
  nlohmann::ordered_json compute_image_stats(const std::vector<cv::Mat> &images) const;

  /**
   * @brief Sample a set of images from a list of image paths
   *
   * @param image_paths Vector of filesystem paths to the images
   * @return Vector of OpenCV Mat objects representing the sampled images
   */
  std::vector<cv::Mat> sample_images(
    const std::vector<std::filesystem::path> &image_paths) const;

  /**
   * @brief Automatically downsample an image to a target size
   *
   * @param img OpenCV Mat object representing the image
   * @param target_size Target size for the downsampled image (default is 150)
   * @param max_threshold Maximum threshold for downsampling (default is 300)
   * @return Downsampled OpenCV Mat object
   */
  cv::Mat auto_downsample(
    const cv::Mat &img,
    int target_size = 150,
    int max_threshold = 300) const;

  /**
   * @brief Sample indices for selecting images from a dataset
   *
   * @param dataset_len Length of the dataset
   * @param min_samples Minimum number of samples to select (default is 100)
   * @param max_samples Maximum number of samples to select (default is 10000)
   * @param power Power factor for sampling distribution (default is 0.75)
   * @return Vector of sampled indices
   */
  std::vector<int> sample_indices(
    int dataset_len,
    int min_samples = 100,
    int max_samples = 10000,
    float power = 0.75f) const;

  /**
   * @brief Update episode information in metadata
   *
   * @param episode_frame_length Length of the episode in frames
   */
  void update_episode_info(int episode_frame_length) const;

  /**
   * @brief Scan directory for existing episode files and return next index
   *
   * @param base_path Directory to scan
   * @return Next episode index (max_found + 1, or 0 if none found)
   */
  static uint32_t scan_existing_episodes(const std::filesystem::path& base_path);

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
     * @return Average encode time in milliseconds
     *
     * @returns Average encode time in milliseconds; 0.0 if no images were written to avoid
     * divide-by-zero
     */
    double avg_encode_ms() const {
      // Avoid divide-by-zero
      if (written == 0) {
        return 0.0;
      }
      return (encode_time_ns_acc / 1e6) / static_cast<double>(written);
    }

    /**
     * @return Average queue wait time in milliseconds
     *
     * @returns Average queue wait time in milliseconds; 0.0 if no images were written to avoid
     * divide-by-zero
     */
    double avg_queue_wait_ms() const {
      if (written == 0) {
        return 0.0;
      }
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
      if (threads == 0 || encode_time_ns_acc == 0) {
        return 0.0;
      }
      // sum of per-frame times across all threads
      double total_s = encode_time_ns_acc / 1e9;
      double frames = static_cast<double>(written);
      // Each frame's encode time counted once; so average frame time = total_s / frames.
      double avg_frame_s = total_s / frames;
      if (avg_frame_s <= 0) {
        return 0.0;
      }
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
  std::shared_ptr<LeRobotBackendConfig> cfg_;

  /// @brief Metadata for this backend
  ProducerMetadataList metadata_;

  /// @brief Store camera names from metadata for easy access
  std::vector<std::string> camera_names_;

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

  /// @brief Hash map to store the frame indices for each source
  std::unordered_map<std::string, uint64_t> source_frame_indices_;

};

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__TROSSEN_HPP
