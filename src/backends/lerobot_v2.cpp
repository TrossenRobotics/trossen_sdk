#include "trossen_sdk/io/backends/lerobot_v2.hpp"

#include <iostream>
#include <sstream>
#include <iomanip>

#include "opencv2/imgcodecs.hpp"

#include "trossen_sdk/data/record.hpp"

// For PNG writing we stub out with raw dump placeholder (future: integrate stb_image_write or libpng)
namespace trossen::io::backends {

// OpenCV PNG compression level (0-9)
// TODO: Make this configurable
const int PNG_COMPRESSION_LEVEL = 3;

namespace fs = std::filesystem;

LeRobotV2Backend::LeRobotV2Backend(const std::string& uri)
  : Backend(uri) {
}

bool LeRobotV2Backend::open() {
  std::lock_guard<std::mutex> lock(open_mutex_);
  if (opened_) {
    return true; // idempotent
  }
  root_ = fs::path(uri_);
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
  // Start image worker
  image_worker_running_ = true;
  image_worker_ = std::thread(&LeRobotV2Backend::imageWorkerLoop, this);
  opened_ = true;
  return true;
}

void LeRobotV2Backend::write(const data::RecordBase& record) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  // Decide type by RTTI (simple approach for now)
  if (auto js = dynamic_cast<const data::JointStateRecord*>(&record)) {
    writeJointState(*js);
  } else if (auto img = dynamic_cast<const data::ImageRecord*>(&record)) {
    writeImage(*img);
  } else {
    // Unknown type ignored for now
  }
}

void LeRobotV2Backend::writeBatch(std::span<const data::RecordBase* const> records) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  for (auto* r : records) {
    if (!r) {
      continue;
    }
    if (auto js = dynamic_cast<const data::JointStateRecord*>(r)) {
      writeJointState(*js);
    } else if (auto img = dynamic_cast<const data::ImageRecord*>(r)) {
      writeImage(*img);
    }
  }
}

void LeRobotV2Backend::flush() {
  if (joint_csv_) joint_csv_.flush();
}

void LeRobotV2Backend::close() {
  std::lock_guard<std::mutex> lock(open_mutex_);
  if (!opened_) return;
  if (joint_csv_.is_open()) joint_csv_.close();
  // Stop image worker
  image_worker_running_ = false;
  image_queue_cv_.notify_all();
  if (image_worker_.joinable()) {
    image_worker_.join();
  }
  opened_ = false;
}

void LeRobotV2Backend::writeJointState(const data::RecordBase& base) {
  const auto& js = static_cast<const data::JointStateRecord&>(base);
  if (!header_written_) {
    joint_csv_ << "monotonic_ns,realtime_ns,id,positions,velocities,efforts\n";
    header_written_ = true;
  }
  auto vecToStr = [](const std::vector<float>& v) {
    std::ostringstream oss;
    for (size_t i=0; i<v.size(); ++i) {
      if (i) oss << ';';
      oss << std::setprecision(6) << v[i];
    }
    return oss.str();
  };
  joint_csv_ << js.ts.monotonic_ns << ','
             << js.ts.realtime_ns << ','
             << js.id << ','
             << vecToStr(js.positions) << ','
             << vecToStr(js.velocities) << ','
             << vecToStr(js.efforts) << '\n';
}

void LeRobotV2Backend::writeImage(const data::RecordBase& base) {
  const auto& img = static_cast<const data::ImageRecord&>(base);

  // exit early if nothing to write
  if (img.image.empty()) {
    return;
  }

  // Directory per camera id (cached)
  // TODO: this could be moved to a pre-processing step if desired
  auto it = image_dir_cache_.find(img.id);
  if (it == image_dir_cache_.end()) {
    fs::path camera_dir = images_root_ / img.id;
    std::error_code ec;
    fs::create_directories(camera_dir, ec);
    it = image_dir_cache_.emplace(img.id, std::move(camera_dir)).first;
  }
  const fs::path& camera_dir = it->second;

  fs::path file_path = camera_dir / (std::to_string(img.ts.monotonic_ns) + ".png");

  // Enqueue job (clone image to ensure safety if producer overwrites buffer later)
  auto job = ImageJob{
    .file_path=file_path,
    .image=img.image.clone()
  };
  {
    std::lock_guard<std::mutex> lk(image_queue_mutex_);
    image_queue_.push_back(std::move(job));
    auto qsize = image_queue_.size();
    img_enqueued_.fetch_add(1, std::memory_order_relaxed);
    size_t prev = img_queue_high_water_.load(std::memory_order_relaxed);
    while (qsize > prev && !img_queue_high_water_.compare_exchange_weak(prev, qsize)) {}
  }
  image_queue_cv_.notify_one();
}

void LeRobotV2Backend::imageWorkerLoop() {
  const std::vector<int> params = { cv::IMWRITE_PNG_COMPRESSION, PNG_COMPRESSION_LEVEL };
  while (image_worker_running_) {
    ImageJob job;
    {
      std::unique_lock<std::mutex> lk(image_queue_mutex_);
      image_queue_cv_.wait(lk, [&]{ return !image_queue_.empty() || !image_worker_running_; });
      if (!image_worker_running_ && image_queue_.empty()) {
        break;
      }
      job = std::move(image_queue_.front());
      image_queue_.pop_front();
    }
    auto t0 = std::chrono::steady_clock::now();
    try {
      if (!cv::imwrite(job.file_path.string(), job.image, params)) {
        std::cerr << "Failed to write PNG: " << job.file_path << std::endl;
      }
    } catch (const cv::Exception& e) {
      std::cerr << "OpenCV exception writing PNG (" << job.file_path << "): " << e.what() << std::endl;
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

} // namespace trossen::io::backends
