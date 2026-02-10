/**
 * @file zed_depth_producer.cpp
 * @brief Implementation of ZedDepthCameraProducer
 */

#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "trossen_sdk/hw/camera/zed_depth_producer.hpp"

namespace trossen::hw::camera {

// FPS tolerance for produced vs requested frame-rate checks
constexpr double FPS_TOLERANCE = 0.5;

ZedDepthCameraProducer::ZedDepthCameraProducer(
  std::shared_ptr<ZedFrameCache> frame_cache, Config cfg)
  : frame_cache_(std::move(frame_cache)),
    cfg_(std::move(cfg)) {
  // Populate metadata
  metadata_.type = "zed_depth_camera";
  metadata_.id = "zed_depth_camera_" + std::to_string(cfg_.serial_number);
  metadata_.name = cfg_.stream_id;
  metadata_.description = "Produces depth frames from ZED camera using ZED SDK";
  metadata_.width = cfg_.width;
  metadata_.height = cfg_.height;
  metadata_.fps = cfg_.fps;
  metadata_.codec = "av1";
  metadata_.pix_fmt = "yuv420p";
  metadata_.channels = 1;  // Depth is single channel
  metadata_.has_audio = false;
  metadata_.is_depth_map = true;

  opened_ = true;
}

ZedDepthCameraProducer::~ZedDepthCameraProducer() {
  if (opened_) {
    std::cout << "ZedDepthCameraProducer: Stopping depth camera: " << metadata_.name << std::endl;
  }
}

void ZedDepthCameraProducer::poll(
  const std::function<void(std::shared_ptr<data::RecordBase>)>& emit)
{
  auto img = std::make_shared<data::ImageRecord>();

  // Use frame cache to grab (shared with color producer)
  sl::ERROR_CODE err = frame_cache_->grab();

  if (err != sl::ERROR_CODE::SUCCESS) {
    if (err != sl::ERROR_CODE::END_OF_SVOFILE_REACHED) {
      std::cerr << "[WARN] ZED depth grab failed: " << sl::toString(err) << std::endl;
    }
    ++stats_.dropped;
    return;
  }

  // Retrieve depth map
  sl::Mat zed_depth;
  auto camera = frame_cache_->get_camera();
  camera->retrieveMeasure(zed_depth, sl::MEASURE::DEPTH);

  // Check if depth map is valid
  if (!zed_depth.isInit() || zed_depth.getWidth() == 0 || zed_depth.getHeight() == 0) {
    std::cerr << "[WARN] ZED retrieved invalid depth map\n";
    ++stats_.dropped;
    return;
  }

  // Convert ZED depth (float32, meters) to 16-bit unsigned (millimeters) for compatibility
  cv::Mat depth_float(zed_depth.getHeight(), zed_depth.getWidth(), CV_32FC1,
                      zed_depth.getPtr<sl::uchar1>(sl::MEM::CPU));

  // Convert from meters to millimeters and then to 16-bit unsigned
  cv::Mat depth_16u;
  depth_float.convertTo(depth_16u, CV_16UC1, 1000.0);  // meters to millimeters

  // Clone the image to ensure we own the data
  img->image = depth_16u.clone();

  // Timestamp handling
  data::Timestamp ts;
  uint64_t mono_now = data::now_mono().to_ns();

  if (cfg_.use_device_time) {
    // Get ZED camera timestamp
    sl::Timestamp zed_ts = camera->getTimestamp(sl::TIME_REFERENCE::IMAGE);
    uint64_t device_ts_ns = zed_ts.getNanoseconds();
    ts.monotonic = data::Timespec::from_ns(device_ts_ns);
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

  // Populate record
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

    if (produced_fps > 0 && (produced_fps + FPS_TOLERANCE) < cfg_.fps) {
      std::cerr << "ZED Depth Camera FPS health: produced_fps=" << produced_fps
                << " requested=" << cfg_.fps
                << " avg_if_ms=" << avg_if_ms
                << " max_if_ms=" << max_if_ms << std::endl;
    } else {
      std::cout << "ZED Depth Camera FPS health: produced_fps=" << produced_fps
                << " requested=" << cfg_.fps
                << " avg_if_ms=" << avg_if_ms
                << " max_if_ms=" << max_if_ms << std::endl;
    }

    // Schedule next report ~10 seconds later
    uint64_t interval = static_cast<uint64_t>(cfg_.fps) * 10;
    if (interval == 0) interval = 300;
    next_health_report_frame_ += interval;
  }
}

}  // namespace trossen::hw::camera
