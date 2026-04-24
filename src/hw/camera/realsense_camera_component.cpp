/**
 * @file realsense_camera_component.cpp
 * @brief Implementation of RealsenseCameraComponent
 */

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

#include "opencv2/opencv.hpp"

#include "trossen_sdk/hw/camera/realsense_camera_component.hpp"
#include "trossen_sdk/hw/discovery_registry.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/utils/camera_utils.hpp"

namespace trossen::hw::camera {

RealsenseCameraComponent::~RealsenseCameraComponent() {
  // TODO(shantanuparab-tr): Properly stop and release Realsense resources
  std::cout << "RealsenseCameraComponent: Destructor called for "
            << get_identifier() << std::endl;
}

void RealsenseCameraComponent::configure(const nlohmann::json& config) {
  // Extract device index (required)
  if (!config.contains("serial_number")) {
    throw std::runtime_error("RealsenseCameraComponent: 'serial_number' is required in config");
  }
  serial_number = config.at("serial_number").get<std::string>();

  // Extract optional parameters
  if (config.contains("width")) {
    width_ = config.at("width").get<int>();
  }
  if (config.contains("height")) {
    height_ = config.at("height").get<int>();
  }
  if (config.contains("fps")) {
    fps_ = config.at("fps").get<int>();
  }
  if (config.contains("use_depth")) {
    use_depth_ = config.at("use_depth").get<bool>();
  }
  if (config.contains("align_depth_to_color")) {
    align_depth_to_color_ = config.at("align_depth_to_color").get<bool>();
  }
  if (config.contains("force_hardware_reset")) {
    force_hardware_reset_ = config.at("force_hardware_reset").get<bool>();
  }

  // TODO(shantanuparab-tr): Improve this logic to avoid iterating all connected devices
  // Force hardware reset if requested
  if (force_hardware_reset_) {
    std::cout << "RealsenseCameraComponent: Forcing hardware reset for camera "
              << serial_number << std::endl;
    // Find the devices connected
    rs2::context ctx;
    auto devices = ctx.query_devices();
    // Reset the device with matching serial number
    for (auto&& dev : devices) {
      if (dev.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER) == serial_number) {
        dev.hardware_reset();
        std::cout << "RealsenseCameraComponent: Hardware reset completed for camera "
                  << serial_number << std::endl;
        break;
      }
    }
  }

  // Create Realsense pipeline
  rs2::pipeline realsense_pipeline;
  auto camera_ = std::make_shared<rs2::pipeline>(realsense_pipeline);

  // Create Realsense config
  rs2::config cam_cfg;

  // Enable the device using the serial number
  cam_cfg.enable_device(serial_number);

  // Enable color stream by default
  cam_cfg.enable_stream(RS2_STREAM_COLOR, width_, height_, RS2_FORMAT_RGB8, fps_);

  // Enable depth stream if requested
  if (use_depth_) {
    cam_cfg.enable_stream(RS2_STREAM_DEPTH, width_, height_, RS2_FORMAT_Z16, fps_);
  }

  // Start the camera pipeline
  try {
    profile = camera_->start(cam_cfg);
  } catch (const rs2::error& e) {
    throw std::runtime_error(
      "RealsenseCameraComponent: Failed to start camera with serial number " +
      serial_number + ": " + std::string(e.what()));
  }

  // Set pipeline_ only after successful start
  pipeline_ = camera_;

  // Store depth scale if depth stream is enabled
  if (use_depth_) {
    try {
      auto depth_sensor = profile.get_device().first<rs2::depth_sensor>();
      depth_scale_ = depth_sensor.get_depth_scale();
    } catch (const rs2::error& e) {
      std::cerr << "RealsenseCameraComponent: Could not read depth scale: "
                << e.what() << std::endl;
      depth_scale_ = 0.001f;  // fallback: 1mm per Z16 unit
    }
  }

  // Read back actual values
  auto color_stream = profile.get_stream(RS2_STREAM_COLOR);
  auto color_profile = color_stream.as<rs2::video_stream_profile>();
  width_ = color_profile.width();
  height_ = color_profile.height();
  fps_ = static_cast<int>(color_profile.fps());

  // Confirm depth stream is valid (uses same width/height as color)
  if (use_depth_) {
    auto depth_stream = profile.get_stream(RS2_STREAM_DEPTH);
    auto depth_profile = depth_stream.as<rs2::video_stream_profile>();
    (void)depth_profile.width();
    (void)depth_profile.height();
  }

  std::cout << "Camera " << get_identifier() << " configured: "
            << width_ << "x" << height_ << " @ " << fps_
            << " FPS" << std::endl;
}

nlohmann::json RealsenseCameraComponent::get_info() const {
  nlohmann::json info = {
    {"type", "realsense_camera"},
    {"serial_number", serial_number},
    {"width", width_},
    {"height", height_},
    {"fps", fps_},
    {"use_depth", use_depth_},
    {"force_hardware_reset", force_hardware_reset_}
  };

  // Add opened status
  info["is_opened"] = is_opened();

  return info;
}

bool RealsenseCameraComponent::is_opened() const {
  if (!pipeline_) {
    return false;
  }
  try {
    pipeline_->get_active_profile();
    return true;
  } catch (const rs2::error&) {
    return false;
  }
}

std::vector<DiscoveredHardware> RealsenseCameraComponent::find(
  const std::filesystem::path& output_dir)
{
  std::vector<DiscoveredHardware> results;

  rs2::context ctx;
  rs2::device_list devices = ctx.query_devices();

  for (uint32_t i = 0; i < devices.size(); ++i) {
    rs2::device dev = devices[i];

    DiscoveredHardware info;
    info.type         = "realsense_camera";
    info.identifier   = dev.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
    info.product_name = dev.get_info(RS2_CAMERA_INFO_NAME);
    info.ok           = false;

    int width  = 640;
    int height = 480;
    int fps    = 30;
    std::filesystem::path preview_path = output_dir / ("realsense_" + info.identifier + ".jpg");

    rs2::pipeline pipeline;
    rs2::config   cfg;
    cfg.enable_device(info.identifier);
    cfg.enable_stream(RS2_STREAM_COLOR, width, height, RS2_FORMAT_BGR8, fps);

    try {
      rs2::pipeline_profile profile = pipeline.start(cfg);

      auto color_profile =
        profile.get_stream(RS2_STREAM_COLOR).as<rs2::video_stream_profile>();
      width  = color_profile.width();
      height = color_profile.height();
      fps    = static_cast<int>(color_profile.fps());

      // Grab frames until one passes the validator (not black, not saturated,
      // not a flat-fill startup buffer) and the auto-exposure has settled.
      cv::Mat best_frame;
      float last_exposure = -1.0f;
      for (int w = 0; w < utils::kPreviewMaxWarmupFrames; ++w) {
        rs2::frameset fs    = pipeline.wait_for_frames();
        rs2::frame    color = fs.get_color_frame();
        if (!color) continue;

        rs2::video_frame vf = color.as<rs2::video_frame>();
        cv::Mat img(
          cv::Size(vf.get_width(), vf.get_height()),
          CV_8UC3,
          const_cast<void*>(color.get_data()),
          cv::Mat::AUTO_STEP);
        if (!utils::is_valid_preview_frame(img)) continue;

        best_frame = img.clone();  // decouple from RealSense's frame buffer
        float exposure = color.get_frame_metadata(RS2_FRAME_METADATA_ACTUAL_EXPOSURE);
        if (last_exposure > 0 &&
            std::abs(exposure - last_exposure) / last_exposure < 0.02f) break;
        last_exposure = exposure;
      }

      if (!best_frame.empty()) {
        info.ok = cv::imwrite(preview_path.string(), best_frame);
        if (!info.ok) {
          std::cerr << "[RealsenseCameraComponent::find] failed to write preview to "
                    << preview_path << "\n";
        }
      } else {
        std::cerr << "[RealsenseCameraComponent::find] " << info.identifier
                  << ": no valid frame captured within " << utils::kPreviewMaxWarmupFrames
                  << " warmup frames\n";
      }
      pipeline.stop();
    } catch (const rs2::error& e) {
      std::cerr << "[RealsenseCameraComponent::find] " << info.identifier << ": "
                << e.what() << "\n";
    }

    info.details = {
      {"width",        width},
      {"height",       height},
      {"fps",          fps},
      {"preview_path", preview_path.string()},
    };
    results.push_back(info);
  }

  return results;
}

REGISTER_HARDWARE(RealsenseCameraComponent, "realsense_camera")
REGISTER_HARDWARE_DISCOVERY(RealsenseCameraComponent, "realsense_camera")

}  // namespace trossen::hw::camera
