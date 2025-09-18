/**
 * @file lerobot_v2.hpp
 * @brief LeRobot V2 backend: writes joint states to CSV and images to directory tree.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2_HPP
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2_HPP

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <thread>
#include <atomic>

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
  LeRobotV2Backend(const std::string& uri);
  bool open() override;
  void write(const data::RecordBase& record) override;
  void writeBatch(std::span<const data::RecordBase* const> records) override;
  void flush() override;
  void close() override;

  struct ImageEncodeStats {
    uint64_t enqueued{0};
    uint64_t encoded{0};
    uint64_t dropped{0};
    uint64_t encode_time_ns_acc{0};
    uint64_t encode_time_ns_max{0};
    size_t queue_high_water{0};
    double avg_encode_ms() const {
      if (encoded == 0) return 0.0;
      return (encode_time_ns_acc / 1e6) / static_cast<double>(encoded);
    }
  };

  ImageEncodeStats image_encode_stats() const {
    ImageEncodeStats s;
    s.enqueued = img_enqueued_.load(std::memory_order_relaxed);
    s.encoded = img_encoded_.load(std::memory_order_relaxed);
    s.dropped = img_dropped_.load(std::memory_order_relaxed);
    s.encode_time_ns_acc = img_encode_time_ns_acc_.load(std::memory_order_relaxed);
    s.encode_time_ns_max = img_encode_time_ns_max_.load(std::memory_order_relaxed);
    s.queue_high_water = img_queue_high_water_.load(std::memory_order_relaxed);
    return s;
  }

private:
  void writeJointState(const data::RecordBase& base);
  void writeImage(const data::RecordBase& base);
  void imageWorkerLoop();

  struct ImageJob {
    std::filesystem::path file_path;
    cv::Mat image; // already cloned for thread safety
  };

  // Async image encoding members
  std::deque<ImageJob> image_queue_;
  std::mutex image_queue_mutex_;
  std::condition_variable image_queue_cv_;
  std::thread image_worker_;
  std::atomic<bool> image_worker_running_{false};

  // Basic stats
  std::atomic<uint64_t> img_enqueued_{0};
  std::atomic<uint64_t> img_encoded_{0};
  std::atomic<uint64_t> img_dropped_{0};
  std::atomic<uint64_t> img_encode_time_ns_acc_{0};
  std::atomic<uint64_t> img_encode_time_ns_max_{0};
  std::atomic<size_t>   img_queue_high_water_{0};

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
