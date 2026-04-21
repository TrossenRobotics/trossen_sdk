/**
 * @file find_cameras.cpp
 * @brief Discover connected cameras and save preview images for identification
 *
 * Enumerates all connected RealSense cameras (via librealsense2) and
 * OpenCV/V4L2 cameras (via /dev/videoN probing), captures one frame from
 * each, and saves previews to an output directory.  Use the preview images
 * to determine which physical camera maps to which serial number or device
 * index, then populate your config files accordingly.

 * Output layout:
 *   <output>/realsense_<serial>.jpg
 *   <output>/opencv_<index>.jpg
 */

#include <algorithm>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include <librealsense2/rs.hpp>
#include <opencv2/opencv.hpp>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct CameraInfo {
  std::string type;        // "realsense" or "opencv"
  std::string identifier;  // serial number or device index string
  int         width;
  int         height;
  int         fps;
  bool        preview_ok;
  fs::path    preview_path;
};

static std::string v4l2_device_name(int index)
{
  fs::path p =
    fs::path("/sys/class/video4linux") / ("video" + std::to_string(index)) / "name";
  std::ifstream f(p);
  if (!f.is_open()) return "";
  std::string name;
  std::getline(f, name);
  return name;
}

static bool is_realsense_v4l2(int index)
{
  std::string name = v4l2_device_name(index);
  std::transform(name.begin(), name.end(), name.begin(), ::tolower);
  return name.find("realsense") != std::string::npos;
}

// ---------------------------------------------------------------------------
// RealSense discovery
// ---------------------------------------------------------------------------

static std::vector<CameraInfo> discover_realsense(const fs::path& output_dir)
{
  std::vector<CameraInfo> results;

  rs2::context    ctx;
  rs2::device_list devices = ctx.query_devices();

  if (devices.size() == 0) {
    std::cout << "  none found\n";
    return results;
  }

  for (uint32_t i = 0; i < devices.size(); ++i) {
    rs2::device dev    = devices[i];
    std::string serial = dev.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
    std::string name   = dev.get_info(RS2_CAMERA_INFO_NAME);

    CameraInfo info;
    info.type         = "realsense";
    info.identifier   = serial;
    info.width        = 640;
    info.height       = 480;
    info.fps          = 30;
    info.preview_ok   = false;
    info.preview_path = output_dir / ("realsense_" + serial + ".jpg");

    rs2::pipeline pipeline;
    rs2::config   cfg;
    cfg.enable_device(serial);
    cfg.enable_stream(RS2_STREAM_COLOR, 640, 480, RS2_FORMAT_BGR8, 30);

    try {
      rs2::pipeline_profile profile = pipeline.start(cfg);

      auto color_profile =
        profile.get_stream(RS2_STREAM_COLOR).as<rs2::video_stream_profile>();
      info.width  = color_profile.width();
      info.height = color_profile.height();
      info.fps    = static_cast<int>(color_profile.fps());

      float last_exposure = -1.0f;
      for (int w = 0; w < 30; ++w) {
        rs2::frameset f     = pipeline.wait_for_frames();
        rs2::frame    color = f.get_color_frame();
        if (color) {
          float exposure = color.get_frame_metadata(RS2_FRAME_METADATA_ACTUAL_EXPOSURE);
          if (last_exposure > 0 &&
              std::abs(exposure - last_exposure) / last_exposure < 0.02f) break;
          last_exposure = exposure;
        }
      }

      rs2::frameset frames = pipeline.wait_for_frames();
      rs2::frame    color  = frames.get_color_frame();
      if (color) {
        rs2::video_frame vf = color.as<rs2::video_frame>();
        cv::Mat img(
          cv::Size(vf.get_width(), vf.get_height()),
          CV_8UC3,
          const_cast<void*>(color.get_data()),
          cv::Mat::AUTO_STEP);
        cv::imwrite(info.preview_path.string(), img);
        info.preview_ok = true;
      }
      pipeline.stop();
    } catch (const rs2::error& e) {
      std::cerr << "  [warn] " << serial << ": " << e.what() << "\n";
    }

    std::cout << "  [" << (info.preview_ok ? "ok  " : "warn") << "] "
              << "realsense  serial=" << serial
              << "  " << info.width << "x" << info.height << " @ " << info.fps << " fps"
              << "  (" << name << ")\n";

    results.push_back(info);
  }

  return results;
}

// ---------------------------------------------------------------------------
// OpenCV / V4L2 discovery
// ---------------------------------------------------------------------------

static std::vector<CameraInfo> discover_opencv(const fs::path& output_dir)
{
  std::vector<CameraInfo> results;
  bool any_found = false;

  for (int idx = 0; idx < 64; ++idx) {
    if (!fs::exists("/dev/video" + std::to_string(idx))) continue;
    if (is_realsense_v4l2(idx)) continue;

    int devnull     = open("/dev/null", O_WRONLY);
    int saved_stderr = dup(2);
    dup2(devnull, 2);
    cv::VideoCapture cap(idx, cv::CAP_V4L2);
    dup2(saved_stderr, 2);
    close(saved_stderr);
    close(devnull);
    if (!cap.isOpened()) {
      cap.release();
      continue;
    }

    any_found = true;
    std::string dev_name = v4l2_device_name(idx);

    CameraInfo info;
    info.type         = "opencv";
    info.identifier   = std::to_string(idx);
    info.width        = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    info.height       = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    info.fps          = static_cast<int>(cap.get(cv::CAP_PROP_FPS));
    info.preview_ok   = false;
    info.preview_path = output_dir / ("opencv_" + std::to_string(idx) + ".jpg");

    cv::Mat frame;
    double last_brightness = -1.0;
    for (int w = 0; w < 30; ++w) {
      cap.read(frame);
      if (frame.empty()) break;
      double brightness = cv::mean(frame)[0];
      if (last_brightness > 0 &&
          std::abs(brightness - last_brightness) / last_brightness < 0.02) break;
      last_brightness = brightness;
    }

    if (!frame.empty()) {
      cv::imwrite(info.preview_path.string(), frame);
      info.preview_ok = true;
    }
    cap.release();

    std::cout << "  [" << (info.preview_ok ? "ok  " : "warn") << "] "
              << "opencv     index=" << idx
              << "  " << info.width << "x" << info.height << " @ " << info.fps << " fps";
    if (!dev_name.empty()) std::cout << "  (" << dev_name << ")";
    std::cout << "\n";

    results.push_back(info);
  }

  if (!any_found) std::cout << "  none found\n";

  return results;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
  fs::path output_dir = "./scripts/find_cameras/camera_discovery";

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help") {
      std::cout << "Usage: " << argv[0] << " [--output DIR]\n";
      std::cout << "\nOptions:\n";
      std::cout << "  --output DIR    Preview output directory (default: ./camera_discovery)\n";
      std::cout << "  --help          Show this help\n";
      std::cout << "\nOutput layout:\n";
      std::cout << "  <output>/realsense_<serial>.jpg\n";
      std::cout << "  <output>/opencv_<index>.jpg\n";
      return 0;
    } else if (arg == "--output" && i + 1 < argc) {
      output_dir = argv[++i];
    }
  }

  fs::create_directories(output_dir);

  std::cout << "\n=== CAMERAS ===\n\n";

  std::cout << "RealSense cameras:\n";
  std::vector<CameraInfo> rs_cameras = discover_realsense(output_dir);

  std::cout << "\nOpenCV / V4L2 cameras:\n";
  std::vector<CameraInfo> cv_cameras = discover_opencv(output_dir);

  std::vector<CameraInfo> all;
  all.insert(all.end(), rs_cameras.begin(), rs_cameras.end());
  all.insert(all.end(), cv_cameras.begin(), cv_cameras.end());

  if (all.empty()) {
    std::cout << "\nNo cameras found.\n";
    return 0;
  }

  // Summary table
  std::cout << "\n"
            << std::left
            << std::setw(12) << "Type"
            << std::setw(22) << "Identifier"
            << std::setw(12) << "Resolution"
            << std::setw(6)  << "FPS"
            << "Preview\n"
            << std::string(80, '-') << "\n";

  for (const auto& cam : all) {
    std::string res = std::to_string(cam.width) + "x" + std::to_string(cam.height);
    std::string fps_str = (cam.fps > 0) ? std::to_string(cam.fps) : "?";
    std::cout << std::left
              << std::setw(12) << cam.type
              << std::setw(22) << cam.identifier
              << std::setw(12) << res
              << std::setw(6)  << fps_str
              << cam.preview_path.string() << "\n";
  }
  std::cout << "\n";

  return 0;
}
