#include "trossen_sdk/hw/camera/opencv_producer.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace trossen::hw::camera {

OpenCvCameraProducer::OpenCvCameraProducer(Config cfg) : cfg_(std::move(cfg)) {
  // Populate metadata
  metadata_.type = "camera";
  metadata_.id = "opencv_camera_" + std::to_string(cfg_.device_index);
  metadata_.name = "OpenCV Camera Producer";
  metadata_.description = "Produces camera frames from a physical camera device using OpenCV";
  metadata_.width = cfg_.width;
  metadata_.height = cfg_.height;
  metadata_.encoding = cfg_.encoding;
  metadata_.fps = cfg_.fps;
}

OpenCvCameraProducer::~OpenCvCameraProducer() {
  if (opened_) {
    cap_.release();
  }
}

bool OpenCvCameraProducer::open_if_needed() {
  if (opened_) return true;
  if (!cap_.open(cfg_.device_index, cv::CAP_V4L2)) {
    std::cerr << "Failed to open camera index " << cfg_.device_index << std::endl;
    return false;
  }
  // Attempt pixel format (FOURCC) negotiation if a preference list was provided.
  // We do this immediately after opening and before setting resolution/FPS so driver
  // can reconfigure cleanly.
  if (!cfg_.preferred_fourcc.empty()) {
    for (size_t i = 0; i < cfg_.preferred_fourcc.size(); ++i) {
      int32_t code = cfg_.preferred_fourcc[i];
      // Break out early if already in desired format (avoid redundant set calls)
      double current_fourcc = cap_.get(cv::CAP_PROP_FOURCC);
      if (static_cast<int32_t>(current_fourcc) == code) {
        break; // already negotiated
      }
      if (!cap_.set(cv::CAP_PROP_FOURCC, static_cast<double>(code))) {
        char c0 = static_cast<char>(code & 0xFF);
        char c1 = static_cast<char>((code >> 8) & 0xFF);
        char c2 = static_cast<char>((code >> 16) & 0xFF);
        char c3 = static_cast<char>((code >> 24) & 0xFF);
        std::cerr << "Failed to set FOURCC=" << c0 << c1 << c2 << c3 << " (index " << i << ")" << std::endl;
        continue;
      }
      // Re-read to verify which format we actually got.
      double after = cap_.get(cv::CAP_PROP_FOURCC);
      if (static_cast<int32_t>(after) == code) {
        char c0 = static_cast<char>(code & 0xFF);
        char c1 = static_cast<char>((code >> 8) & 0xFF);
        char c2 = static_cast<char>((code >> 16) & 0xFF);
        char c3 = static_cast<char>((code >> 24) & 0xFF);
        std::cout << "Negotiated FOURCC=" << c0 << c1 << c2 << c3 << " (preference " << i << ")" << std::endl;
        break; // stop after first successful preference
      }
    }
  }
  if (cfg_.width > 0 && !cap_.set(cv::CAP_PROP_FRAME_WIDTH, cfg_.width)) {
    std::cerr << "Failed to set width=" << cfg_.width << std::endl; return false; }
  if (cfg_.height > 0 && !cap_.set(cv::CAP_PROP_FRAME_HEIGHT, cfg_.height)) {
    std::cerr << "Failed to set height=" << cfg_.height << std::endl; return false; }
  if (cfg_.fps > 0 && !cap_.set(cv::CAP_PROP_FPS, cfg_.fps)) {
    std::cerr << "Failed to set fps=" << cfg_.fps << std::endl; return false; }

  double eff_w = cap_.get(cv::CAP_PROP_FRAME_WIDTH);
  double eff_h = cap_.get(cv::CAP_PROP_FRAME_HEIGHT);
  double eff_fps = cap_.get(cv::CAP_PROP_FPS);
  double fourcc = cap_.get(cv::CAP_PROP_FOURCC);
  char fourcc_chars[] = {
    static_cast<char>(static_cast<int>(fourcc) & 0xFF),
    static_cast<char>((static_cast<int>(fourcc) >> 8) & 0xFF),
    static_cast<char>((static_cast<int>(fourcc) >> 16) & 0xFF),
    static_cast<char>((static_cast<int>(fourcc) >> 24) & 0xFF),
    '\0'};
  std::cout << "Camera opened: " << eff_w << "x" << eff_h << " @ " << eff_fps
            << " FPS FOURCC=" << fourcc_chars << std::endl;
  if (cfg_.fps > 0 && static_cast<int>(eff_fps) != cfg_.fps) {
    std::cerr << "Warning: requested fps=" << cfg_.fps << " but device reports " << eff_fps << std::endl;
  }
  opened_ = true;
  return true;
}

bool OpenCvCameraProducer::warmup() {
  // TODO: check that received frames are valid and match the requested configuration
  if (!open_if_needed()) {
    return false;
  }
  if (cfg_.warmup_seconds <= 0.0) {
    return true;
  }
  auto warmup_end = std::chrono::steady_clock::now() + std::chrono::duration<double>(cfg_.warmup_seconds);
  cv::Mat frame;
  while (std::chrono::steady_clock::now() < warmup_end) {
    if (!cap_.read(frame)) {
      ++stats_.dropped;
      continue;
    }
    ++stats_.warmup_discarded;
    // Sleep a bit to avoid tight busy loop
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

void OpenCvCameraProducer::poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) {
  // TODO: silently fails if cannot open
  if (!open_if_needed()) {
    return;
  }
  auto img = std::make_shared<data::ImageRecord>();
  if (!cap_.read(img->image)) {
    ++stats_.dropped;
    return;
  }

  data::Timestamp ts;
  // TODO: use device timestamp if available and cfg_.use_device_time
  uint64_t mono_now = data::now_mono().to_ns();
  ts.monotonic = data::now_mono();
  ts.realtime = data::now_real();

  // Inter-frame timing instrumentation
  if (last_capture_mono_ != 0) {
    uint64_t dt = mono_now - last_capture_mono_;
    if_accum_ns_ += dt;
    if (dt > if_max_ns_) if_max_ns_ = dt;
    ++if_samples_;
  }
  last_capture_mono_ = mono_now;

  img->ts = ts;
  img->seq = seq_++;
  img->id = cfg_.stream_id;
  img->width = static_cast<uint32_t>(img->image.cols);
  img->height = static_cast<uint32_t>(img->image.rows);
  img->encoding = cfg_.encoding;
  img->channels = static_cast<uint32_t>(img->image.channels());

  emit(img);
  ++stats_.produced;

  // Periodic FPS health report every approx requested_fps * 10 frames (or default 300) if requested fps set
  // TODO: make this all configurable via config
  if (cfg_.fps > 0 && stats_.produced >= next_health_report_frame_) {
    double avg_if_ms = 0.0;
    double max_if_ms = 0.0;
    if (if_samples_ > 0) {
      avg_if_ms = (if_accum_ns_ / 1e6) / static_cast<double>(if_samples_);
      max_if_ms = if_max_ns_ / 1e6;
    }
    double produced_fps = 0.0;
    if (if_samples_ > 0 && if_accum_ns_ > 0) {
      produced_fps = 1e9 / (static_cast<double>(if_accum_ns_) / static_cast<double>(if_samples_));
    }
    if (produced_fps > 0 && (produced_fps + 0.5) < cfg_.fps) {
      std::cerr << "Camera FPS health: produced_fps=" << produced_fps << " requested=" << cfg_.fps
                << " avg_if_ms=" << avg_if_ms << " max_if_ms=" << max_if_ms << std::endl;
    } else {
      std::cout << "Camera FPS health: produced_fps=" << produced_fps << " requested=" << cfg_.fps
                << " avg_if_ms=" << avg_if_ms << " max_if_ms=" << max_if_ms << std::endl;
    }
    // schedule next report
    uint64_t interval = static_cast<uint64_t>(cfg_.fps) * 10; // ~10 seconds
    if (interval == 0) interval = 300;
    next_health_report_frame_ += interval;
  }
}

} // namespace trossen::hw::camera
