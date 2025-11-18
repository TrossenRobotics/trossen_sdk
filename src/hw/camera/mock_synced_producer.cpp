/**
 * @file mock_synced_producer.cpp
 */
#include "trossen_sdk/hw/camera/mock_synced_producer.hpp"

#include <algorithm>

namespace trossen::hw::camera {

using namespace std::chrono;
using trossen::data::now_mono;
using trossen::data::now_real;

static inline std::string color_id(const std::string& cam){ return std::string("/cameras/") + cam + "/color/image"; }
static inline std::string depth_id(const std::string& cam){ return std::string("/cameras/") + cam + "/depth/image"; }

MockSyncedCameraProducer::MockSyncedCameraProducer(Config cfg) : cfg_(std::move(cfg)) {
  cfg_.streams.normalize();
  if (cfg_.seed) rng_.seed(static_cast<uint32_t>(cfg_.seed));
  warmup_remaining_ = cfg_.streams.warmup_frames;
  frame_period_ns_ = cfg_.streams.frame_period_ns();

  // Populate metadata
  metadata_.type = "camera";
  metadata_.id = "mock_camera_" + std::to_string(cfg_.seed);
  metadata_.name = "Mock Synchronized Camera Producer";
  metadata_.description = "Produces synthetic synchronized color and depth frames for testing and diagnostics";
  metadata_.width = 640;
  metadata_.height = 480;
  metadata_.fps = 30;
  metadata_.codec = "av1";
  metadata_.pix_fmt = "yuv420p";
  metadata_.channels = 3;
  metadata_.camera_name = "mock_synced_camera";
  metadata_.has_audio = false;
  metadata_.is_mock = false;

}

void MockSyncedCameraProducer::poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) {
  uint64_t now_ns = now_mono().to_ns();
  if (last_emit_mono_ != 0) {
    uint64_t dt = now_ns - last_emit_mono_;
    if (intervals_ns_.size() < kMaxIntervals) intervals_ns_.push_back(dt);
    if (frame_period_ns_ > 0 && dt < frame_period_ns_) {
      return; // not time yet
    }
  }

  // Capture timestamps once for pair
  auto ts_mono = now_mono();
  auto ts_real = now_real();
  uint64_t seq = seq_++;

  // Generate color
  cv::Mat color_mat(cfg_.streams.color_height, cfg_.streams.color_width, CV_8UC3);
  generate_color(color_mat, seq);

  if (warmup_remaining_ > 0) {
    --warmup_remaining_;
    last_emit_mono_ = now_ns;
    return; // discard both (color+depth) during warmup
  }

  // Emit color record
  {
    auto rec = std::make_shared<data::ImageRecord>();
    rec->ts.monotonic = ts_mono;
    rec->ts.realtime = ts_real;
    rec->seq = seq;
    rec->id = color_id(cfg_.camera_name);
    rec->width = static_cast<uint32_t>(color_mat.cols);
    rec->height = static_cast<uint32_t>(color_mat.rows);
    rec->channels = static_cast<uint32_t>(color_mat.channels());
    rec->encoding = cfg_.streams.color_encoding; // e.g. bgr8
    rec->image = std::move(color_mat);
    emit(rec);
  }

  if (depth_enabled(cfg_.streams.capability)) {
    // Prepare depth
    cv::Mat depth_mat;
    std::string depth_encoding;
    if (cfg_.streams.depth_format == DepthFormat::DEPTH16) {
      depth_mat.create(cfg_.streams.depth_height, cfg_.streams.depth_width, CV_16UC1);
      generate_depth_u16(depth_mat, seq);
      depth_encoding = "depth16"; // canonical
    } else { // FLOAT32
      depth_mat.create(cfg_.streams.depth_height, cfg_.streams.depth_width, CV_32FC1);
      generate_depth_f32(depth_mat, seq);
      depth_encoding = "32FC1";
    }

    auto rec = std::make_shared<data::ImageRecord>();
    rec->ts.monotonic = ts_mono; // identical timestamps
    rec->ts.realtime = ts_real;
    rec->seq = seq;              // shared sequence
    rec->id = depth_id(cfg_.camera_name);
    rec->width = static_cast<uint32_t>(depth_mat.cols);
    rec->height = static_cast<uint32_t>(depth_mat.rows);
    rec->channels = 1; // depth is single channel
    rec->encoding = depth_encoding;
    rec->image = std::move(depth_mat);
    emit(rec);
  }

  last_emit_mono_ = now_ns;
}

void MockSyncedCameraProducer::generate_color(cv::Mat &dst, uint64_t seq_counter) {
  if (cfg_.pattern == Pattern::Gradient) {
    double t = (seq_counter % 1000) / 1000.0;
    for (int y=0; y<dst.rows; ++y) {
      auto *row = dst.ptr<cv::Vec3b>(y);
      for (int x=0; x<dst.cols; ++x) {
        uint8_t v = static_cast<uint8_t>((255.0 * x) / dst.cols);
        uint8_t b = static_cast<uint8_t>(v * (0.5 + 0.5 * t));
        row[x] = cv::Vec3b(b, v, static_cast<uint8_t>((v + y) & 0xFF));
      }
    }
  } else { // Noise
    for (int y=0; y<dst.rows; ++y) {
      auto *row = dst.ptr<cv::Vec3b>(y);
      for (int x=0; x<dst.cols; ++x) {
        float n0 = norm_(rng_) * cfg_.noise_stddev + 127.f;
        float n1 = norm_(rng_) * cfg_.noise_stddev + 127.f;
        float n2 = norm_(rng_) * cfg_.noise_stddev + 127.f;
        row[x] = cv::Vec3b(
          static_cast<uint8_t>(std::clamp(n0, 0.f, 255.f)),
          static_cast<uint8_t>(std::clamp(n1, 0.f, 255.f)),
          static_cast<uint8_t>(std::clamp(n2, 0.f, 255.f))
        );
      }
    }
  }
}

void MockSyncedCameraProducer::generate_depth_u16(cv::Mat &dst, uint64_t seq_counter) {
  // Deterministic ramp pattern: value cycles to keep in a plausible range (~0..5000 mm)
  uint16_t base = static_cast<uint16_t>((seq_counter * 13) % 5000);
  for (int y=0; y<dst.rows; ++y) {
    uint16_t *row = dst.ptr<uint16_t>(y);
    for (int x=0; x<dst.cols; ++x) {
      row[x] = static_cast<uint16_t>((base + x + y) % 5000);
    }
  }
}

void MockSyncedCameraProducer::generate_depth_f32(cv::Mat &dst, uint64_t seq_counter) {
  float base = static_cast<float>((seq_counter * 0.013));
  for (int y=0; y<dst.rows; ++y) {
    float *row = dst.ptr<float>(y);
    for (int x=0; x<dst.cols; ++x) {
      row[x] = 0.5f + 0.001f * static_cast<float>((x + y) % 3000) + base;
    }
  }
}

MockSyncedCameraProducer::JitterStats MockSyncedCameraProducer::jitter_stats() const {
  JitterStats js;
  if (intervals_ns_.empty()) return js;
  std::vector<uint64_t> copy = intervals_ns_;
  std::sort(copy.begin(), copy.end());
  double sum = 0.0;
  for (auto v : copy) sum += static_cast<double>(v);
  auto pct = [&](double p){ size_t idx = static_cast<size_t>(p * (copy.size()-1)); return copy[idx]; };
  js.samples = copy.size();
  js.mean_ms = (sum / copy.size()) / 1e6;
  js.p50_ms = pct(0.50) / 1e6;
  js.p95_ms = pct(0.95) / 1e6;
  js.p99_ms = pct(0.99) / 1e6;
  js.max_ms = copy.back() / 1e6;
  return js;
}

} // namespace trossen::hw::camera
