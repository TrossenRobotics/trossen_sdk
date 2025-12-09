/**
 * @file mock_producer.cpp
 * @brief Implementation of synthetic camera frame producer.
 */

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "trossen_sdk/hw/camera/mock_producer.hpp"

namespace trossen::hw::camera {


MockCameraProducer::MockCameraProducer(Config cfg) : cfg_(std::move(cfg)) {
  rng_.seed(static_cast<uint32_t>(cfg_.seed));
  warmup_remaining_ = cfg_.warmup_frames;

  // Populate metadata
  metadata_.type = "mock_camera";
  metadata_.id = cfg_.stream_id;
  metadata_.name = cfg_.stream_id;
  metadata_.description = "Produces synthetic camera frames for testing and diagnostics";
  metadata_.width = cfg_.width;
  metadata_.height = cfg_.height;
  metadata_.fps = cfg_.fps;
  metadata_.codec = "av1";
  metadata_.pix_fmt = "yuv420p";
  metadata_.channels = (cfg_.encoding == "bgr8" || cfg_.encoding == "rgb8") ? 3 : 1;
  metadata_.has_audio = false;

  // Pre-allocate frame buffer to avoid repeated allocations
  frame_buffer_ = cv::Mat(cfg_.height, cfg_.width, CV_8UC3);

  // Generate cached frame if caching is enabled
  if (cfg_.cache_frames) {
    cached_frame_ = cv::Mat(cfg_.height, cfg_.width, CV_8UC3);
    generate_frame(cached_frame_);
    cache_valid_ = true;
  }
}

void MockCameraProducer::poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) {
  uint64_t now_mono = data::now_mono().to_ns();

  // Warmup: discard first N emission opportunities without generating frames
  // This allows the system to stabilize before recording actual data
  if (warmup_remaining_ > 0) {
    if (cache_valid_) {
      // Use cached frame for warmup
    } else {
      generate_frame(frame_buffer_);
    }
    --warmup_remaining_;
    ++stats_.warmup_discarded;
    last_emit_mono_ = now_mono;
    return;
  }

  // Simulated drop
  if (cfg_.drop_probability > 0.0 && drop_dist_(rng_) < cfg_.drop_probability) {
    ++stats_.dropped;
    last_emit_mono_ = now_mono;
    return;
  }

  // Record interval for emitted frames only
  if (last_emit_mono_ != 0) {
    uint64_t dt = now_mono - last_emit_mono_;
    if (intervals_ns_.size() < kMaxIntervals) intervals_ns_.push_back(dt);
  }

  // Use cached frame or generate into pre-allocated buffer
  cv::Mat* source_frame;
  if (cache_valid_) {
    source_frame = &cached_frame_;
  } else {
    generate_frame(frame_buffer_);
    source_frame = &frame_buffer_;
  }

  auto rec = std::make_shared<data::ImageRecord>();
  rec->ts.monotonic = data::now_mono();
  rec->ts.realtime = data::now_real();
  rec->seq = seq_++;
  rec->id = cfg_.stream_id;
  rec->width = static_cast<uint32_t>(source_frame->cols);
  rec->height = static_cast<uint32_t>(source_frame->rows);
  rec->channels = static_cast<uint32_t>(source_frame->channels());
  rec->encoding = cfg_.encoding;
  rec->image = source_frame->clone();  // Clone for record (must own the data)

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
  auto pct = [&](double p) {
    size_t idx = static_cast<size_t>(p * (copy.size()-1));
    return copy[idx];
  };
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
    case Pattern::Solid: {
      // Solid gray
      dst.setTo(cv::Scalar(128, 128, 128));
      break;
    }
    case Pattern::Gradient: {
      // Pre-compute one row, memcpy to all rows
      static std::vector<cv::Vec3b> gradient_row;
      if (gradient_row.size() != static_cast<size_t>(dst.cols)) {
        gradient_row.resize(dst.cols);
        for (int x = 0; x < dst.cols; ++x) {
          uint8_t v = static_cast<uint8_t>((255.0 * x) / dst.cols);
          gradient_row[x] = cv::Vec3b(v/2, v, v);
        }
      }
      // Copy pre-computed row to all rows
      for (int y = 0; y < dst.rows; ++y) {
        std::memcpy(dst.ptr<cv::Vec3b>(y), gradient_row.data(),
                    dst.cols * sizeof(cv::Vec3b));
      }
      break;
    }
    case Pattern::Noise: {
      for (int y=0; y < dst.rows; ++y) {
        auto *row = dst.ptr<cv::Vec3b>(y);
        for (int x=0; x < dst.cols; ++x) {
          double n0 = noise_norm_(rng_) * cfg_.noise_stddev + 127.0;
          double n1 = noise_norm_(rng_) * cfg_.noise_stddev + 127.0;
          double n2 = noise_norm_(rng_) * cfg_.noise_stddev + 127.0;
          row[x] = cv::Vec3b(
            static_cast<uint8_t>(std::clamp(n0, 0.0, 255.0)),
            static_cast<uint8_t>(std::clamp(n1, 0.0, 255.0)),
            static_cast<uint8_t>(std::clamp(n2, 0.0, 255.0)));
        }
      }
      break;
    }
  }
}

}  // namespace trossen::hw::camera
