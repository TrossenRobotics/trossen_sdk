/**
 * @file mock_producer.cpp
 */

#include "trossen_sdk/hw/camera/mock_producer.hpp"

#include <chrono>
#include <iostream>

namespace trossen::hw::camera {

using namespace std::chrono;

MockCameraProducer::MockCameraProducer(Config cfg) : cfg_(std::move(cfg)) {
  rng_.seed(static_cast<uint32_t>(cfg_.seed));
  warmup_remaining_ = cfg_.warmup_frames;
  if (cfg_.fps > 0) {
    frame_period_ns_ = static_cast<uint64_t>(1e9 / static_cast<double>(cfg_.fps));
  }

  // Populate metadata
  metadata_.id = cfg_.stream_id;
  metadata_.name = "Mock Camera Producer";
  metadata_.description = "Produces synthetic camera frames for testing and diagnostics";
  metadata_.width = cfg_.width;
  metadata_.height = cfg_.height;
  metadata_.encoding = cfg_.encoding;
  metadata_.fps = cfg_.fps;
}

void MockCameraProducer::poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) {
  // Respect target FPS (if configured)
  uint64_t now_mono = data::now_mono().to_ns();
  if (last_emit_mono_ != 0) {
    uint64_t dt = now_mono - last_emit_mono_;
    if (intervals_ns_.size() < kMaxIntervals) intervals_ns_.push_back(dt);
  }
  if (frame_period_ns_ > 0 && last_emit_mono_ != 0) {
    if (now_mono - last_emit_mono_ < frame_period_ns_) {
      return; // not time yet
    }
  }

  // Simulated drop
  if (cfg_.drop_probability > 0.0 && drop_dist_(rng_) < cfg_.drop_probability) {
    ++stats_.dropped; // treat as drop
    last_emit_mono_ = now_mono; // still advance to keep pacing realistic
    return;
  }

  cv::Mat frame(cfg_.height, cfg_.width, CV_8UC3);
  generate_frame(frame);

  if (warmup_remaining_ > 0) {
    --warmup_remaining_;
    ++stats_.warmup_discarded;
    last_emit_mono_ = now_mono;
    return;
  }

  auto rec = std::make_shared<data::ImageRecord>();
  rec->ts.monotonic = data::now_mono();
  rec->ts.realtime = data::now_real();
  rec->seq = seq_++;
  rec->id = cfg_.stream_id;
  rec->width = static_cast<uint32_t>(frame.cols);
  rec->height = static_cast<uint32_t>(frame.rows);
  rec->channels = static_cast<uint32_t>(frame.channels());
  rec->encoding = cfg_.encoding;
  rec->image = std::move(frame);

  emit(rec);
  ++stats_.produced;
  last_emit_mono_ = now_mono;
}

MockCameraProducer::JitterStats MockCameraProducer::jitter_stats() const {
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

void MockCameraProducer::generate_frame(cv::Mat &dst) {
  switch (cfg_.pattern) {
    case Pattern::Gradient: {
      // Horizontal gradient with vertical modulation (time-based to create subtle motion)
      double t = (seq_ % 1000) / 1000.0;
      for (int y=0; y<dst.rows; ++y) {
        auto *row = dst.ptr<cv::Vec3b>(y);
        for (int x=0; x<dst.cols; ++x) {
          uint8_t v = static_cast<uint8_t>((255.0 * x) / dst.cols);
          uint8_t b = static_cast<uint8_t>(v * (0.5 + 0.5 * t));
          row[x] = cv::Vec3b(b, v, static_cast<uint8_t>((v + y) & 0xFF));
        }
      }
      break;
    }
    case Pattern::Noise: {
      for (int y=0; y<dst.rows; ++y) {
        auto *row = dst.ptr<cv::Vec3b>(y);
        for (int x=0; x<dst.cols; ++x) {
          double n0 = noise_norm_(rng_) * cfg_.noise_stddev + 127.0;
          double n1 = noise_norm_(rng_) * cfg_.noise_stddev + 127.0;
          double n2 = noise_norm_(rng_) * cfg_.noise_stddev + 127.0;
          row[x] = cv::Vec3b(
            static_cast<uint8_t>(std::clamp(n0, 0.0, 255.0)),
            static_cast<uint8_t>(std::clamp(n1, 0.0, 255.0)),
            static_cast<uint8_t>(std::clamp(n2, 0.0, 255.0))
          );
        }
      }
      break;
    }
  }
}

} // namespace trossen::hw::camera
