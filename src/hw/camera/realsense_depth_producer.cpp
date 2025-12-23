/**
 * @file realsense_depth_producer.cpp
 * @brief Implementation of RealsenseDepthCameraProducer that emits camera frames from a physical camera
 * device using Realsense SDK.
 */

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "trossen_sdk/hw/camera/realsense_depth_producer.hpp"

namespace trossen::hw::camera {

RealsenseDepthCameraProducer::RealsenseDepthCameraProducer(
  std::shared_ptr<trossen::hw::camera::RealsenseFrameCache> frame_cache, Config cfg)
  : frame_cache_(std::move(frame_cache)),
    cfg_(std::move(cfg)) {
  // Populate metadata
  metadata_.type = "realsense_camera";
  metadata_.id = "realsense_camera_" + cfg_.serial_number;
  metadata_.name = cfg_.stream_id;
  metadata_.description = "Produces camera frames from a physical camera device using Realsense";
  metadata_.width = cfg_.width;
  metadata_.height = cfg_.height;
  metadata_.fps = cfg_.fps;
  metadata_.codec = "av1";
  metadata_.pix_fmt = "yuv420p";
  metadata_.channels = (cfg_.encoding == "bgr8" || cfg_.encoding == "rgb8") ? 3 : 1;
  metadata_.has_audio = false;
}

RealsenseDepthCameraProducer::~RealsenseDepthCameraProducer() {
  if (opened_) {
    std::cout << "Stopping RealSense camera: " << metadata_.name << std::endl;
  }
}

void RealsenseDepthCameraProducer::poll(
  const std::function<void(std::shared_ptr<data::RecordBase>)>& emit)
{
  auto img = std::make_shared<data::ImageRecord>();

  rs2::frameset frames;
  try {
    frames = frame_cache_->get_frames(3000);
  } catch (const rs2::error& e) {
    std::cout << "[WARN] RealSense poll failed: " << e.what() << std::endl;
    ++stats_.dropped;
    return;
  }

  auto depth_frame = frames.get_depth_frame();
  if (!depth_frame) {
    std::cout << "[WARN] RealSense poll failed: " << std::endl;

    ++stats_.dropped;
    return;
  }

  // Convert rs2::frame to cv::Mat
  const void* data_ptr = static_cast<const void*>(depth_frame.get_data());
  cv::Mat depth_image(cv::Size(cfg_.width, cfg_.height), CV_16UC1,
                    const_cast<void*>(data_ptr), cv::Mat::AUTO_STEP);
  img->image = std::move(depth_image);

  data::Timestamp ts;
  // TODO(lukeschmitt-tr): use device timestamp if available and cfg_.use_device_time

  uint64_t mono_now = data::now_mono().to_ns();

  if (cfg_.use_device_time) {
    uint64_t device_ts = depth_frame.get_timestamp();
    ts.monotonic = data::Timespec::from_ns(device_ts);
  } else {
    ts.monotonic = data::now_mono();
  }
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

  // Periodically report FPS health

  // TODO(lukeschmitt-tr): make this all configurable via config
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
      std::cout << "Camera FPS health: produced_fps=" << produced_fps << " requested=" << cfg_.fps
                << " avg_if_ms=" << avg_if_ms << " max_if_ms=" << max_if_ms << std::endl;
    } else {
      std::cout << "Camera FPS health: produced_fps=" << produced_fps << " requested=" << cfg_.fps
                << " avg_if_ms=" << avg_if_ms << " max_if_ms=" << max_if_ms << std::endl;
    }
    // schedule next report ~10 seconds later
    uint64_t interval = static_cast<uint64_t>(cfg_.fps) * 10;
    if (interval == 0) interval = 300;
    next_health_report_frame_ += interval;
  }
}

}  // namespace trossen::hw::camera
