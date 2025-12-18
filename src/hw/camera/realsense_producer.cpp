/**
 * @file realsense_producer.cpp
 * @brief Implementation of RealsenseCameraProducer that emits camera frames from a physical camera
 * device using Realsense.
 */

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "trossen_sdk/hw/camera/realsense_producer.hpp"

namespace trossen::hw::camera {

RealsenseCameraProducer::RealsenseCameraProducer(Config cfg) : cfg_(std::move(cfg)) {
  // Populate metadata
  metadata_.type = "realsense_camera";
  metadata_.id = "realsense_camera_" + std::to_string(cfg_.serial_number);
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

RealsenseCameraProducer::~RealsenseCameraProducer() {
  if (opened_) {
    cap_.release();
  }
}

bool RealsenseCameraProducer::open_if_needed() {
  if (opened_) return true;
  // Create a realsense config
  rs2::config cam_cfg;

  // Enable the device using the unique ID
  if (!unique_id_.empty()) {
    cam_cfg.enable_device(cfg_.serial_number);
  } else {
    spdlog::error(
        "Unique ID is empty. Cannot connect to RealSense camera: {}", cfg_.stream_id);
    throw std::runtime_error("Unique ID is empty for camera: " + cfg_.stream_id);
  }

  // Enable the color stream as default
  cam_cfg.enable_stream(RS2_STREAM_COLOR, cfg_.width, cfg_.height,
                    RS2_FORMAT_RGB8, cfg_.fps);

  // Enable depth stream if use_depth_ is true
  if (use_depth_) {
    cam_cfg.enable_stream(RS2_STREAM_DEPTH, cfg_.width, cfg_.height,
                      RS2_FORMAT_Z16, cfg_.fps);
  }

  try {
    // Start the camera pipeline
    rs2::pipeline_profile profile = camera_.start(cam_cfg);
  } catch (const rs2::error& e) {
    spdlog::error(
        "Failed to enable device with Unique ID: {}. Error: {}. Listing all "
        "available cameras.",
        unique_id_, e.what());
    find_cameras();
    spdlog::info(
        "Available cameras listed above. Please check outputs folder to get "
        "images associated "
        "with each camera.");
    throw std::runtime_error(
        "Unique ID (serial number) is required to connect to RealSense camera");
  }
  opened_ = true;
  return true;
}



void RealsenseCameraProducer::poll(
  const std::function<void(std::shared_ptr<data::RecordBase>)>& emit)
{
  // TODO(lukeschmitt-tr): silently fails if cannot open
  if (!open_if_needed()) {
    return;
  }
  auto img = std::make_shared<data::ImageRecord>();

  // Initialize frameset
  rs2::frameset frames;

  // Wait for the next set of frames from the camera with a timeout of 3000 ms
  try {
    frames = camera_.wait_for_frames(3000);
  } catch (const rs2::error& e) {
    std::cout << "[WARN] Failed to get frames from RealSense camera: " << e.what() << std::endl;
    ++stats_.dropped;
    return;
  }

  if (frames && frames.size() == 0) {
    spdlog::error("No frames received from camera: {}", name_);
    return;
  }
  // Try to get color frame
  try {
    rs2::frame color_frame = frames.get_color_frame();

    if (color_frame) {
      // Convert rs2::frame to cv::Mat
      const void* data_ptr = static_cast<const void*>(color_frame.get_data());
      cv::Mat image(cv::Size(capture_width_, capture_height_), CV_8UC3,
                    const_cast<void*>(data_ptr), cv::Mat::AUTO_STEP);
      img_data->image = std::move(image);
    } else {
      spdlog::error("Failed to read color frame from camera: {}", name_);
    }
  } catch (const rs2::error& e) {
    spdlog::error("Error retrieving color frame from camera: {}. Error: {}",
                  name_, e.what());
  }
  if (cfg_.use_depth_) {
    // Try to get depth frame
    try {
      rs2::frame depth_frame = frames.get_depth_frame();
      if (depth_frame) {
        const void* depth_data_ptr =
            static_cast<const void*>(depth_frame.get_data());
        cv::Mat depth_map(cv::Size(capture_width_, capture_height_), CV_16UC1,
                          const_cast<void*>(depth_data_ptr),
                          cv::Mat::AUTO_STEP);
        img_data->depth_map = std::move(depth_map);
      } else {
        spdlog::error("Failed to read depth frame from camera: {}", name_);
      }
    } catch (const rs2::error& e) {
      spdlog::error("Error retrieving depth frame from camera: {}. Error: {}",
                    name_, e.what());
    }
  }
  img_data->timestamp_ms = static_cast<int64_t>(frames.get_timestamp());

  data::Timestamp ts;
  // TODO(lukeschmitt-tr): use device timestamp if available and cfg_.use_device_time
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
      std::cerr << "Camera FPS health: produced_fps=" << produced_fps << " requested=" << cfg_.fps
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
