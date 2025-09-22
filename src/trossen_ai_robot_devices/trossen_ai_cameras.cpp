// Copyright 2025 Trossen Robotics
#include "trossen_ai_robot_devices/trossen_ai_cameras.hpp"

namespace trossen_ai_robot_devices {

RealsenseCamera::RealsenseCamera(const std::string& name,
                                 const std::string& unique_id,
                                 int capture_width, int capture_height, int fps,
                                 bool use_depth)
    : TrossenAICamera(name, unique_id, capture_width, capture_height, fps,
                      use_depth) {
  // Initialize camera settings if needed
  spdlog::info(
      "RealsenseCamera initialized: name={}, unique_id={}, width={}, "
      "height={}, fps={}, "
      "use_depth={}",
      name_, unique_id_, capture_width_, capture_height_, fps_, use_depth_);
}

void RealsenseCamera::connect() {
  // Create a realsense config
  rs2::config cfg;

  // Enable the device using the unique ID
  if (!unique_id_.empty()) {
    cfg.enable_device(unique_id_);
  }

  // Enable the color stream as default
  cfg.enable_stream(RS2_STREAM_COLOR, capture_width_, capture_height_,
                    RS2_FORMAT_RGB8, fps_);

  // Enable depth stream if use_depth_ is true
  if (use_depth_) {
    cfg.enable_stream(RS2_STREAM_DEPTH, capture_width_, capture_height_,
                      RS2_FORMAT_Z16, fps_);
  }

  // Start the camera pipeline with the configuration
  rs2::pipeline_profile profile = camera_.start(cfg);
}

void RealsenseCamera::disconnect() {
  // TODO(shantanuparab-tr): Properly stop the camera pipeline if needed
  // Log disconnection
  spdlog::info("Disconnecting from camera: {}", name_);
}

trossen_ai_robot_devices::ImageData RealsenseCamera::read() {
  // Initialize frameset
  rs2::frameset frames;

  // Wait for the next set of frames from the camera with a timeout of 3000 ms
  try {
    frames = camera_.wait_for_frames(3000);
  } catch (const rs2::error& e) {
    spdlog::error("Failed to get frameset from camera: {}. Error: {}", name_,
                  e.what());
    return trossen_ai_robot_devices::ImageData{};
  }

  // Extract color and depth frames
  trossen_ai_robot_devices::ImageData data;
  data.camera_name = name_;
  if (frames && frames.size() == 0) {
    spdlog::error("No frames received from camera: {}", name_);
    return data;
  }
  // Try to get color frame
  try {
    rs2::frame color_frame = frames.get_color_frame();

    if (color_frame) {
      // Convert rs2::frame to cv::Mat
      const void* data_ptr = static_cast<const void*>(color_frame.get_data());
      cv::Mat image(cv::Size(capture_width_, capture_height_), CV_8UC3,
                    const_cast<void*>(data_ptr), cv::Mat::AUTO_STEP);
      data.image = image;
    } else {
      spdlog::error("Failed to read color frame from camera: {}", name_);
    }
  } catch (const rs2::error& e) {
    spdlog::error("Error retrieving color frame from camera: {}. Error: {}",
                  name_, e.what());
  }
  if (use_depth_) {
    // Try to get depth frame
    try {
      rs2::frame depth_frame = frames.get_depth_frame();
      if (depth_frame) {
        const void* depth_data_ptr =
            static_cast<const void*>(depth_frame.get_data());
        cv::Mat depth_map(cv::Size(capture_width_, capture_height_), CV_16UC1,
                          const_cast<void*>(depth_data_ptr),
                          cv::Mat::AUTO_STEP);
        data.depth_map = depth_map;
      } else {
        spdlog::error("Failed to read depth frame from camera: {}", name_);
      }
    } catch (const rs2::error& e) {
      spdlog::error("Error retrieving depth frame from camera: {}. Error: {}",
                    name_, e.what());
    }
  }
  data.timestamp_ms = static_cast<int64_t>(frames.get_timestamp());
  return data;
}

void RealsenseCamera::find_cameras() {
  // Create a context object. This object owns the handles to all connected
  // realsense devices.
  rs2::context ctx;
  auto devices =
      ctx.query_devices();  // Get a snapshot of currently connected devices
  if (devices.size() == 0) {
    spdlog::warn("No RealSense cameras found.");
    return;
  }

  // Create a thread pool for asynchronous image saving
  AsyncImageWriter image_writer(4);  // Using 4 threads for image saving

  for (const auto& dev : devices) {
    std::string serial = dev.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
    std::string name = dev.get_info(RS2_CAMERA_INFO_NAME);
    spdlog::info("Found camera: {} with Serial Number: {}", name, serial);

    // Create a temporary camera instance to capture an image
    RealsenseCamera temp_camera(name, serial, capture_width_, capture_height_,
                                fps_, use_depth_);
    temp_camera.connect();
    auto image_data = temp_camera.read();
    temp_camera.disconnect();

    if (!image_data.image.empty()) {
      // Prepare filename using the serial number
      std::string filename = serial + ".png";

      // Enqueue the image saving task
      image_writer.push(ImageSaveTask{image_data.image, filename});
      spdlog::info("Enqueued image saving for camera: {} to file: {}", name,
                   filename);
    } else {
      spdlog::error("Failed to capture image from camera: {}", name);
    }
  }
}

OpenCVCamera::OpenCVCamera(const std::string& name,
                           const std::string& unique_id, int capture_width,
                           int capture_height, int fps, bool use_depth)
    : TrossenAICamera(name, unique_id, capture_width, capture_height, fps,
                      use_depth) {
  // Initialize camera settings if needed
  spdlog::info(
      "OpenCVCamera initialized: name={}, unique_id={}, width={}, height={}, "
      "fps={}, "
      "use_depth={}",
      name_, unique_id_, capture_width_, capture_height_, fps_, use_depth_);
}

void OpenCVCamera::connect() {
  // Open the camera using OpenCV VideoCapture
  int device_index = -1;
  try {
    device_index = std::stoi(unique_id_);
  } catch (const std::invalid_argument& e) {
    spdlog::error(
        "Invalid unique_id for OpenCVCamera: {}. Must be an integer index.",
        unique_id_);
    return;
  }

  cv::VideoCapture cap(device_index);
  if (!cap.isOpened()) {
    spdlog::error("Failed to open camera with index: {}", device_index);
    // Run find_cameras to list available cameras
    find_cameras();
    // TODO(shantanuparab-tr) Store the output images in a
    // known location and inform the user
    spdlog::info(
        "Available cameras listed above. Please check outputs folder to get "
        "images associated "
        "with each camera.");
    throw std::runtime_error("Failed to open camera with index: " +
                             std::to_string(device_index));
  }

  // Set camera properties
  cap.set(cv::CAP_PROP_FRAME_WIDTH, capture_width_);
  cap.set(cv::CAP_PROP_FRAME_HEIGHT, capture_height_);
  cap.set(cv::CAP_PROP_FPS, fps_);

  // Store the VideoCapture object in a member variable
  video_capture_ = std::move(cap);

  spdlog::info("Connected to OpenCVCamera: {}", name_);
}

void OpenCVCamera::disconnect() {
  if (video_capture_.isOpened()) {
    video_capture_.release();
    spdlog::info("Disconnected from OpenCVCamera: {}", name_);
  } else {
    spdlog::warn("OpenCVCamera: {} was not connected.", name_);
  }
}

trossen_ai_robot_devices::ImageData OpenCVCamera::read() {
  trossen_ai_robot_devices::ImageData data;
  data.camera_name = name_;

  if (!video_capture_.isOpened()) {
    spdlog::error("Camera not connected: {}", name_);
    return data;
  }

  cv::Mat frame;
  if (!video_capture_.read(frame)) {
    spdlog::error("Failed to read frame from camera: {}", name_);
    return data;
  }
  // Convert BGR to RGB
  cv::cvtColor(frame, frame, cv::COLOR_BGR2RGB);
  data.image = frame;
  data.timestamp_ms =
      static_cast<int64_t>(cv::getTickCount() / cv::getTickFrequency() *
                           1000);  // Approximate timestamp in ms

  // Note: OpenCV does not natively support depth maps, so this is left empty
  if (use_depth_) {
    spdlog::warn("Depth map requested but not supported in OpenCVCamera: {}",
                 name_);
  }

  return data;
}

trossen_ai_robot_devices::ImageData OpenCVCamera::async_read() {
  trossen_ai_robot_devices::ImageData result;
  std::mutex mtx;
  std::condition_variable cv;
  bool done = false;

  // Launch a thread to read the image asynchronously
  std::thread([this, &result, &mtx, &cv, &done]() {
    // Read color frame
    trossen_ai_robot_devices::ImageData image_data = read();
    // Lock and set the result
    {
      std::lock_guard<std::mutex> lock(mtx);
      result = image_data;
      done = true;
    }
    // Notify the waiting thread
    cv.notify_one();
  }).detach();

  // Wait for the image to be read or timeout after 5 seconds
  std::unique_lock<std::mutex> lock(mtx);
  if (!cv.wait_for(lock, std::chrono::seconds(5), [&done] { return done; })) {
    spdlog::error("Timeout: Failed to receive image within 5 seconds.");
    // Optionally, set result to an empty image or error state
    result = trossen_ai_robot_devices::ImageData{};
  }
  return result;
}

void OpenCVCamera::find_cameras() {
  std::vector<std::string> targets_to_scan;

  // Scan /dev for video devices
  for (const auto& entry : std::filesystem::directory_iterator("/dev")) {
    if (entry.path().filename().string().find("video") == 0) {
      targets_to_scan.push_back(entry.path().string());
    }
  }

  // Sort to ensure consistent order
  std::sort(targets_to_scan.begin(), targets_to_scan.end());

  for (const std::string& path : targets_to_scan) {
    cv::VideoCapture cap(path, cv::CAP_V4L2);
    if (cap.isOpened()) {
      double width = cap.get(cv::CAP_PROP_FRAME_WIDTH);
      double height = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
      double fps = cap.get(cv::CAP_PROP_FPS);
      double format = cap.get(cv::CAP_PROP_FORMAT);

      if (width > 0 && height > 0) {
        spdlog::info("Found camera at {}: {}x{} @ {} FPS, format={}", path,
                     width, height, fps, format);
        // Save 1 image from each camera with the unique_id as the filename.
        // Warm up the camera by reading 50 frames
        cv::Mat frame;
        for (int i = 0; i < 50; ++i) {
          if (!cap.read(frame)) {
            spdlog::debug("Failed to read frame {} from {}", i, path);
            break;
          }
        }
        // Save the 50th frame if successfully read
        if (!frame.empty()) {
          std::string filename =
              path.substr(path.find_last_of('/') + 1) + ".png";
          if (cv::imwrite(filename, frame)) {
            spdlog::debug("Saved image from {} to {}", path, filename);
          } else {
            spdlog::debug("Failed to save image from {}", path);
          }
        } else {
          spdlog::debug("Failed to read the 50th frame from {}", path);
        }

        cap.release();
      }
    }
  }
}

trossen_ai_robot_devices::ImageData RealsenseCamera::async_read() {
  trossen_ai_robot_devices::ImageData result;
  std::mutex mtx;
  std::condition_variable cv;
  bool done = false;

  // Launch a thread to read the image asynchronously
  std::thread([this, &result, &mtx, &cv, &done]() {
    // Read color and depth frames
    trossen_ai_robot_devices::ImageData image_data = read();
    // Lock and set the result
    {
      std::lock_guard<std::mutex> lock(mtx);
      result = image_data;
      done = true;
    }
    // Notify the waiting thread
    cv.notify_one();
  }).detach();

  // Wait for the image to be read or timeout after 5 seconds
  std::unique_lock<std::mutex> lock(mtx);
  if (!cv.wait_for(lock, std::chrono::seconds(5), [&done] { return done; })) {
    spdlog::error("Timeout: Failed to receive image within 5 seconds.");
    // Optionally, set result to an empty image or error state
    result = trossen_ai_robot_devices::ImageData{};
  }
  return result;
}

AsyncImageWriter::AsyncImageWriter(int num_threads)
    : stop_flag_(false), num_threads_(num_threads) {
  // Create worker threads
  for (int i = 0; i < num_threads_; ++i) {
    worker_threads_.emplace_back(&AsyncImageWriter::worker_loop, this);
  }
}

AsyncImageWriter::~AsyncImageWriter() {
  // Signal all threads to stop
  {
    std::lock_guard<std::mutex> lock(mtx_);
    stop_flag_ = true;
  }
  cv_.notify_all();
  // Join all threads
  for (auto& thread : worker_threads_) {
    if (thread.joinable()) thread.join();
  }
}

void AsyncImageWriter::push(ImageSaveTask task) {
  // Add image and filename to the queue
  {
    std::lock_guard<std::mutex> lock(mtx_);
    image_queue_.emplace(std::move(task));  // ensure data safety
  }
  cv_.notify_one();
}

void AsyncImageWriter::worker_loop() {
  // Worker thread loop
  while (true) {
    // Wait for an image to be available or stop signal
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this]() { return stop_flag_ || !image_queue_.empty(); });

    if (stop_flag_ && image_queue_.empty()) break;

    // Get the next image and filename from the queue
    ImageSaveTask task = std::move(image_queue_.front());
    image_queue_.pop();
    lock.unlock();
    if (task.image.empty()) {
      spdlog::debug("Image is empty, skipping: {}", task.filename);
      continue;
    }

    cv::Mat image_to_write;
    // Check the channel count to distinguish depth from color
    if (task.image.channels() == 1) {
      // If the image is already CV_16UC1, no need to convert
      if (task.image.type() != CV_16UC1) {
        // TODO(shantanuparab-tr) [TDS-36]: Handle different depth formats
        // if necessary. Convert to CV_16UC1 assuming the input is
        // in meters (float) and we want millimeters (int)
        task.image.convertTo(image_to_write, CV_16UC1, 1000);
      } else {
        image_to_write = task.image;
      }
    } else {
      if (task.image.channels() == 3) {
        // Convert RGB to BGR for OpenCV
        cv::cvtColor(task.image, image_to_write, cv::COLOR_RGB2BGR);
      } else if (task.image.channels() == 4) {
        // Convert RGBA to BGR for OpenCV
        cv::cvtColor(task.image, image_to_write, cv::COLOR_RGBA2BGR);
      } else {
        // Unexpected, but save as is
        image_to_write = task.image;
      }
    }
    try {
      // Check if the parent directory exists, if not create it
      std::filesystem::path file_path(task.filename);
      std::filesystem::path parent_dir = file_path.parent_path();
      if (!std::filesystem::exists(parent_dir)) {
        std::filesystem::create_directories(parent_dir);
      }
      if (!cv::imwrite(task.filename, image_to_write)) {
        spdlog::error("Failed to write image to: {}", task.filename);
      }
    } catch (const cv::Exception& e) {
      spdlog::error("Exception while writing image to {}: {}", task.filename,
                    e.what());
    }
  }
}

}  // namespace trossen_ai_robot_devices
