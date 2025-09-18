#include "trossen_sdk/hw/camera/opencv_producer.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace trossen::hw::camera {

OpenCvCameraProducer::OpenCvCameraProducer(Config cfg) : cfg_(std::move(cfg)) {}

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
  opened_ = true;
  return true;
}

bool OpenCvCameraProducer::warmup() {
  if (!open_if_needed()) return false;
  if (cfg_.warmup_seconds <= 0.0) return true;
  auto warmup_end = std::chrono::steady_clock::now() +
    std::chrono::duration<double>(cfg_.warmup_seconds);
  cv::Mat frame;
  while (std::chrono::steady_clock::now() < warmup_end) {
    if (!cap_.read(frame)) {
      ++stats_.dropped;
      continue;
    }
    ++stats_.warmup_discarded;
    std::this_thread::sleep_for(std::chrono::milliseconds(1)); // avoid tight busy loop
  }
  return true;
}

void OpenCvCameraProducer::poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) {
  if (!open_if_needed()) return; // silent fail if cannot open
  cv::Mat frame;
  if (!cap_.read(frame)) { ++stats_.dropped; return; }

  data::Timestamp ts;
  uint64_t mono_now = data::now_mono_ns();
  ts.monotonic_ns = mono_now;
  ts.realtime_ns = data::now_real_ns();

  auto img = std::make_shared<data::ImageRecord>();
  img->ts = ts;
  img->seq = seq_++;
  img->id = cfg_.stream_id;
  img->width = static_cast<uint32_t>(frame.cols);
  img->height = static_cast<uint32_t>(frame.rows);
  img->encoding = cfg_.encoding;
  img->channels = static_cast<uint32_t>(frame.channels());

  size_t bytes = frame.total() * frame.elemSize();
  auto buffer = std::make_shared<std::vector<uint8_t>>(bytes);
  std::memcpy(buffer->data(), frame.data, bytes);
  img->data = buffer;

  emit(img);
  ++stats_.produced;
}

} // namespace trossen::hw::camera
