#include "trossen_ai_robot_devices/trossen_ai_cameras.hpp"

namespace trossen_ai_robot_devices {

TrossenAICamera::TrossenAICamera(const std::string& name, const std::string& serial_number, 
                                 int capture_width, int capture_height, int fps, bool use_depth)
    : name_(name), serial_number_(serial_number), capture_width_(capture_width), capture_height_(capture_height), fps_(fps), use_depth_(use_depth) {
    // Initialize camera settings if needed
    spdlog::info("TrossenAICamera initialized: name={}, serial={}, width={}, height={}, fps={}, use_depth={}", 
                 name_, serial_number_, capture_width_, capture_height_, fps_, use_depth_);
}

void TrossenAICamera::connect() {
    
    // Create a realsense config
    rs2::config cfg;

    // Enable the device using the serial number
    if (!serial_number_.empty()) {
        cfg.enable_device(serial_number_);
    }

    // Enable the color stream as default
    cfg.enable_stream(
        RS2_STREAM_COLOR, capture_width_, capture_height_, RS2_FORMAT_RGB8, fps_);

    // Enable depth stream if use_depth_ is true
    if (use_depth_) {
        cfg.enable_stream(
            RS2_STREAM_DEPTH, capture_width_, capture_height_, RS2_FORMAT_Z16, fps_);
    }

    // Start the camera pipeline with the configuration
    rs2::pipeline_profile profile = camera_.start(cfg);
}


void TrossenAICamera::disconnect() {
    // TODO: Properly stop the camera pipeline if needed
    // Log disconnection
    spdlog::info("Disconnecting from camera: {}", name_);
}

trossen_ai_robot_devices::ImageData TrossenAICamera::read() {

    // Initialize frameset
    rs2::frameset frames;

    // Wait for the next set of frames from the camera with a timeout of 3000 ms
    try {
        frames = camera_.wait_for_frames(3000); 
    } catch (const rs2::error& e) {
        spdlog::error("Failed to get frameset from camera: {}. Error: {}", name_, e.what());
        return trossen_ai_robot_devices::ImageData{};
    }

    // Extract color and depth frames
    trossen_ai_robot_devices::ImageData data;
    data.camera_name = name_;
    if (frames && frames.size() > 0) {
        // Try to get color frame
        try {
            rs2::frame color_frame = frames.get_color_frame();

            if (color_frame) {
                // Convert rs2::frame to cv::Mat
                cv::Mat image(cv::Size(capture_width_, capture_height_), CV_8UC3, (void*)color_frame.get_data(), cv::Mat::AUTO_STEP);
                data.image = image;
            } else {
                spdlog::error("Failed to read color frame from camera: {}", name_);
            }
        } catch (const rs2::error& e) {
            spdlog::error("Error retrieving color frame from camera: {}. Error: {}", name_, e.what());
        }
        if (use_depth_){
            // Try to get depth frame
            try {
                rs2::frame depth_frame = frames.get_depth_frame();
                if (depth_frame) {
                    cv::Mat depth_map = cv::Mat(cv::Size(capture_width_, capture_height_), CV_16UC1, (void*)depth_frame.get_data(), cv::Mat::AUTO_STEP);
                    data.depth_map = depth_map;
                } else {
                    spdlog::error("Failed to read depth frame from camera: {}", name_);
                }
            } catch (const rs2::error& e) {
                spdlog::error("Error retrieving depth frame from camera: {}. Error: {}", name_, e.what());
            }
        }
        data.timestamp_ms = static_cast<int64_t>(frames.get_timestamp());
    } else {
        spdlog::error("Frameset is empty for camera: {}", name_);
    }
    return data;
}



trossen_ai_robot_devices::ImageData TrossenAICamera::async_read() {

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
    if (!cv.wait_for(lock, std::chrono::seconds(5), [&done]{ return done; })) {
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

void AsyncImageWriter::push(const cv::Mat& image, const std::string& filename) {
    // Add image and filename to the queue
    {
        std::lock_guard<std::mutex> lock(mtx_);
        image_queue_.emplace(image.clone(), filename);  // ensure data safety
    }
    cv_.notify_one();
}

void AsyncImageWriter::worker_loop() {
    // Worker thread loop
    while (true) {
        // Wait for an image to be available or stop signal
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this]() {
            return stop_flag_ || !image_queue_.empty();
        });

        if (stop_flag_ && image_queue_.empty())
            break;

        // Get the next image and filename from the queue
        auto [image, filename] = std::move(image_queue_.front());
        image_queue_.pop();
        lock.unlock();
        if(image.empty()) {
            spdlog::debug("Depth image is empty, skipping: {}", filename);
            continue;
        }

        cv::Mat image_to_write;
        // Check the channel count to distinguish depth from color
        if (image.channels() == 1) {
            // If the image is already CV_16UC1, no need to convert
            if (image.type() != CV_16UC1) {
                // TODO [TDS-36]: Handle different depth formats if necessary
                // Convert to CV_16UC1 assuming the input is in meters (float) and we want millimeters (int)
                image.convertTo(image_to_write, CV_16UC1, 1000);
            } else {
                image_to_write = image;
            }
        } else {
            if (image.channels() == 3) {
                // Convert RGB to BGR for OpenCV
                cv::cvtColor(image, image_to_write, cv::COLOR_RGB2BGR);
            } else if (image.channels() == 4) {
                // Convert RGBA to BGR for OpenCV
                cv::cvtColor(image, image_to_write, cv::COLOR_RGBA2BGR);
            } else {
                // Unexpected, but save as is
                image_to_write = image;
            }
        }
        try {
                // Check if the parent directory exists, if not create it
                std::filesystem::path file_path(filename);
                std::filesystem::path parent_dir = file_path.parent_path();
                if (!std::filesystem::exists(parent_dir)) {
                    std::filesystem::create_directories(parent_dir);
                }
                if (!cv::imwrite(filename, image_to_write)) {
                    spdlog::error("Failed to write image to: {}", filename);
                }
            } catch (const cv::Exception& e) {
                spdlog::error("Exception while writing image to {}: {}", filename, e.what());
            }
    }
}

} // namespace trossen_data_collection_sdk