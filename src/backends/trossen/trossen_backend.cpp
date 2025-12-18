/**
 * @file trossen_backend.cpp
 * @brief Implementation of TrossenBackend for Trossen SDK.
 */

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "opencv2/imgcodecs.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/io/backend_registry.hpp"
#include "trossen_sdk/io/backends/trossen/trossen_backend.hpp"

namespace trossen::io::backends {

REGISTER_BACKEND(TrossenBackend, "trossen")

namespace fs = std::filesystem;

TrossenBackend::TrossenBackend(
  Config cfg,
  const ProducerMetadataList&)
  : io::Backend(), cfg_(std::move(cfg))
{
  // Validate encoder threads
  if (cfg_.encoder_threads <= 0) {
    cfg_.encoder_threads = 1;
  }
  // Validate PNG compression level
  if (cfg_.png_compression_level < 0 || cfg_.png_compression_level > 9) {
    cfg_.png_compression_level = 3;
  }

  // URI is absolute path to output directory. Set to absolute path and check write access.
  // Validate output directory path
  try {
    root_ = fs::absolute(cfg_.output_dir);
  } catch (const std::exception& e) {
    throw std::runtime_error(
      "Failed to resolve absolute path of output directory: " + std::string(e.what()));
  }
  // Validate non-empty path
  if (root_.empty()) {
    throw std::runtime_error("Output directory path is empty");
  }
  // Check that we can write to the output directory
  try {
    if (!fs::exists(root_)) {
      fs::create_directories(root_);
    }
  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to create output directory: " + std::string(e.what()));
  }
  fs::path test_path = root_ / "trossen_sdk_lerobot_v2_test.tmp";
  std::ofstream test_file(test_path.string(), std::ios::out | std::ios::trunc);
  if (!test_file) {
    throw std::runtime_error("Failed to open test file for writing: " + test_path.string());
  }
  test_file.close();
  fs::remove(test_path);

  // Print off configuration
  std::cout << "TrossenBackend configuration:\n"
            << "  Output dir: " << root_ << "\n"
            << "  Encoder threads: " << cfg_.encoder_threads << "\n"
            << "  Max image queue: "
            << (cfg_.max_image_queue == 0 ? "unbounded" : std::to_string(cfg_.max_image_queue))
            << "\n"
            << "  Drop policy: "
            << (cfg_.drop_policy == DropPolicy::DropNewest ? "DropNewest" :
                (cfg_.drop_policy == DropPolicy::DropOldest ? "DropOldest" : "Block"))
            << "\n"
            << "  PNG compression level: " << cfg_.png_compression_level << "\n";
}

bool TrossenBackend::open() {
  std::lock_guard<std::mutex> lock(open_mutex_);
  if (opened_) {
    return true;
  }
  images_root_ = root_ / "observations" / "images";
  try {
    fs::create_directories(images_root_);
  } catch (const std::exception& e) {
    std::cerr << "Failed to create directories: " << e.what() << "\n";
    return false;
  }
  joint_csv_.open((root_ / "joint_states.csv").string(), std::ios::out | std::ios::trunc);
  if (!joint_csv_) {
    std::cerr << "Failed to open joint_states.csv\n";
    return false;
  }
  header_written_ = false;
  // Cache queue policy derived members
  max_image_queue_cached_ = cfg_.max_image_queue;

  // Start image worker threads
  image_worker_running_ = true;
  size_t nthreads = cfg_.encoder_threads == 0 ? 1 : cfg_.encoder_threads;
  image_workers_.reserve(nthreads);
  for (size_t i = 0; i < nthreads; ++i) {
    image_workers_.emplace_back(&TrossenBackend::image_worker_loop, this);
  }
  opened_ = true;
  return true;
}

void TrossenBackend::write(const data::RecordBase& record) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  // Decide type by RTTI (simple approach for now)
  if (auto js = dynamic_cast<const data::JointStateRecord*>(&record)) {
    write_joint_state(*js);
  } else if (auto img = dynamic_cast<const data::ImageRecord*>(&record)) {
    write_image(*img);
  } else {
    // Unknown type ignored for now
  }
}

void TrossenBackend::write_batch(std::span<const data::RecordBase* const> records) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  for (auto* r : records) {
    if (!r) {
      continue;
    }
    if (auto js = dynamic_cast<const data::JointStateRecord*>(r)) {
      write_joint_state(*js);
    } else if (auto img = dynamic_cast<const data::ImageRecord*>(r)) {
      write_image(*img);
    }
  }
}

void TrossenBackend::flush() {
  if (joint_csv_) joint_csv_.flush();
}

void TrossenBackend::close() {
  std::lock_guard<std::mutex> lock(open_mutex_);
  if (!opened_) return;
  if (joint_csv_.is_open()) joint_csv_.close();
  // Stop image worker
  image_worker_running_ = false;
  image_queue_cv_.notify_all();
  for (auto &t : image_workers_) {
    if (t.joinable()) t.join();
  }
  image_workers_.clear();
  opened_ = false;
}

void TrossenBackend::write_joint_state(const data::RecordBase& base) {
  const auto& js = static_cast<const data::JointStateRecord&>(base);
  if (!header_written_) {
    joint_csv_ << "monotonic_ns,realtime_ns,id,positions,velocities,efforts\n";
    header_written_ = true;
  }
  auto vecToStr = [](const std::vector<float>& v) {
    std::ostringstream oss;
    for (size_t i=0; i < v.size(); ++i) {
      if (i) oss << ';';
      oss << std::setprecision(6) << v[i];
    }
    return oss.str();
  };
  joint_csv_ << js.ts.monotonic.to_ns() << ','
             << js.ts.realtime.to_ns() << ','
             << js.id << ','
             << vecToStr(js.positions) << ','
             << vecToStr(js.velocities) << ','
             << vecToStr(js.efforts) << '\n';
}

void TrossenBackend::write_image(const data::RecordBase& base) {
  const auto& img = static_cast<const data::ImageRecord&>(base);

  // exit early if nothing to write
  if (img.image.empty()) {
    return;
  }

  // Directory per camera id (cached)
  // TODO(lukeschmitt-tr): this could be moved to a pre-processing step?
  auto it = image_dir_cache_.find(img.id);
  if (it == image_dir_cache_.end()) {
    fs::path camera_dir = images_root_ / img.id;
    std::error_code ec;
    fs::create_directories(camera_dir, ec);
    it = image_dir_cache_.emplace(img.id, std::move(camera_dir)).first;
  }
  const fs::path& camera_dir = it->second;

  fs::path file_path = camera_dir / (std::to_string(img.ts.monotonic.to_ns()) + ".png");

  // Enqueue job (clone image to ensure safety if producer overwrites buffer later)
  //
  // Zero-copy enqueue: we rely on cv::Mat's internal ref counting. The producer will have its own
  // shared_ptr to the ImageRecord; backend stores another cv::Mat header referencing same data.
  // When encoding finishes and job.image goes out of scope, the buffer will be released if no
  // other references remain.
  auto job = ImageJob{ file_path, img.image };
  auto enqueue_time = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lk(image_queue_mutex_);
    // Enforce queue policy if max_image_queue_ is set
    if (max_image_queue_cached_ > 0 && image_queue_.size() >= max_image_queue_cached_) {
      switch (cfg_.drop_policy) {
        case DropPolicy::DropNewest: {
          // Silently drop newest by not adding it to the queue
          img_dropped_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        case DropPolicy::DropOldest: {
          // Drop the oldest in the queue to make room for the new one
          if (!image_queue_.empty()) {
            image_queue_.pop_front();
            img_dropped_.fetch_add(1, std::memory_order_relaxed);
          }
          // Break to add the new job
          break;
        }
        // case DropPolicy::Block:
        //   // Wait until space is available (not implemented)
        //   image_queue_cv_.wait(lk, [&]{ return image_queue_.size() < max_image_queue_cached_; });
        //   break;
      }
    }
  // Extend job by storing enqueue steady time via file_path metadata hack? Better: expand struct.
  image_queue_.push_back(std::move(job));
  // Store enqueue timestamp in parallel deque
  image_queue_enqueue_times_.push_back(enqueue_time);
    auto qsize = image_queue_.size();
    img_enqueued_.fetch_add(1, std::memory_order_relaxed);
    size_t prev = img_queue_high_water_.load(std::memory_order_relaxed);
    while (qsize > prev && !img_queue_high_water_.compare_exchange_weak(prev, qsize)) {}
    img_queue_backlog_sum_.fetch_add(qsize, std::memory_order_relaxed);
    img_queue_backlog_samples_.fetch_add(1, std::memory_order_relaxed);
  }
  image_queue_cv_.notify_one();
}

void TrossenBackend::image_worker_loop() {
  const std::vector<int> params = { cv::IMWRITE_PNG_COMPRESSION, cfg_.png_compression_level };
  while (image_worker_running_) {
    ImageJob job;
    {
      std::unique_lock<std::mutex> lk(image_queue_mutex_);
      image_queue_cv_.wait(lk, [&]{ return !image_queue_.empty() || !image_worker_running_; });
      if (!image_worker_running_ && image_queue_.empty()) {
        break;
      }
      job = std::move(image_queue_.front());
      auto enq_time = image_queue_enqueue_times_.front();
      image_queue_enqueue_times_.pop_front();
      image_queue_.pop_front();
      // queue wait time
      auto start_steady = std::chrono::steady_clock::now();
      uint64_t wait_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(start_steady - enq_time).count();
      img_queue_wait_time_ns_acc_.fetch_add(wait_ns, std::memory_order_relaxed);
      uint64_t prev_qw_max = img_queue_wait_time_ns_max_.load(std::memory_order_relaxed);
      while (
        wait_ns > prev_qw_max
        && !img_queue_wait_time_ns_max_.compare_exchange_weak(prev_qw_max, wait_ns))
      {
        // loop
      }
    }
    auto t0 = std::chrono::steady_clock::now();
    try {
      if (!cv::imwrite(job.file_path.string(), job.image, params)) {
        std::cerr << "Failed to write PNG: " << job.file_path << std::endl;
      }
    } catch (const cv::Exception& e) {
      std::cerr << "OpenCV exception writing PNG ("
                << job.file_path << "): " << e.what() << std::endl;
    }
    auto t1 = std::chrono::steady_clock::now();
    uint64_t dt = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    img_encode_time_ns_acc_.fetch_add(dt, std::memory_order_relaxed);
    // update max
    uint64_t prev_max = img_encode_time_ns_max_.load(std::memory_order_relaxed);
    while (dt > prev_max && !img_encode_time_ns_max_.compare_exchange_weak(prev_max, dt)) {}
    img_encoded_.fetch_add(1, std::memory_order_relaxed);
  }
  // Drain leftover jobs on shutdown
  while (true) {
    ImageJob job;
    {
      std::lock_guard<std::mutex> lk(image_queue_mutex_);
      if (image_queue_.empty()) break;
      job = std::move(image_queue_.front());
      image_queue_.pop_front();
    }
    try {
      cv::imwrite(job.file_path.string(), job.image);
      img_encoded_.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
      // ignore - we don't care to write images were shutting down anyway
    }
  }
}

}  // namespace trossen::io::backends
