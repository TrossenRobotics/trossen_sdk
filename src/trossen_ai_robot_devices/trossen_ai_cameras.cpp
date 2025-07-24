#include "trossen_ai_robot_devices/trossen_ai_cameras.hpp"

namespace trossen_ai_robot_devices {

TrossenAICamera::TrossenAICamera(const std::string& name, const std::string& serial_number, 
                                 int capture_width, int capture_height, int fps)
    : name_(name), serial_number_(serial_number), capture_width_(capture_width), capture_height_(capture_height), fps_(fps) {
    // Initialize camera settings if needed
    std::cout << "TrossenAICamera initialized with name: " << name_
              << ", serial number: " << serial_number_
              << ", width: " << capture_width_
              << ", height: " << capture_height_
              << ", fps: " << fps_ << std::endl;
}
void TrossenAICamera::connect() {
    // Connect to the camera
   // Add connection logic here
    rs2::config cfg;
    if (!serial_number_.empty()) {
        cfg.enable_device(serial_number_);
    }

    cfg.enable_stream(
                RS2_STREAM_COLOR, capture_width_, capture_height_, RS2_FORMAT_RGB8, fps_
            );
    cfg.enable_stream(
                RS2_STREAM_DEPTH, capture_width_, capture_height_, RS2_FORMAT_Z16, fps_
            );
    rs2::pipeline_profile profile = camera_.start(cfg);
    // Add more configuration options as needed
    // Start the camera pipeline

}

void TrossenAICamera::disconnect() {
    // Disconnect from the camera
    std::cout << "Disconnecting from camera: " << name_ << std::endl;
    // Add disconnection logic here
}

trossen_ai_robot_devices::ColorDepthData TrossenAICamera::read() {
    // Read a frame from the camera
    rs2::frameset frames;
    try {
        frames = camera_.wait_for_frames(3000); // Wait for a frame for up to 5000 ms
    } catch (const rs2::error& e) {
        std::cerr << "Failed to get frameset from camera: " << name_ << ". Error: " << e.what() << std::endl;
        return trossen_ai_robot_devices::ColorDepthData{};
    }

    trossen_ai_robot_devices::ColorDepthData data;
    if (frames && frames.size() > 0) {
        // Try to get color frame
        try {
            rs2::frame color_frame = frames.get_color_frame();
            if (color_frame) {
                data.color_image = color_frame;
            } else {
                std::cerr << "Failed to read color frame from camera: " << name_ << std::endl;
            }
        } catch (const rs2::error& e) {
            std::cerr << "Error retrieving color frame from camera: " << name_ << ". Error: " << e.what() << std::endl;
        }

        // Try to get depth frame
        try {
            rs2::frame depth_frame = frames.get_depth_frame();
            if (depth_frame) {
                data.depth_map = depth_frame;
            } else {
                std::cerr << "Failed to read depth frame from camera: " << name_ << std::endl;
            }
        } catch (const rs2::error& e) {
            std::cerr << "Error retrieving depth frame from camera: " << name_ << ". Error: " << e.what() << std::endl;
        }
    } else {
        std::cerr << "Frameset is empty for camera: " << name_ << std::endl;
    }
    return data;
}



trossen_ai_robot_devices::ImageData TrossenAICamera::async_read() {
    // Start an asynchronous read thread
    // Note: In a real implementation, you would use a separate thread to read frames asynchronously
    // For simplicity, we will use the synchronous read method here.
    trossen_ai_robot_devices::ImageData result;
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;

    std::thread([this, &result, &mtx, &cv, &done]() {
        trossen_ai_robot_devices::ColorDepthData color_and_depth = read();  // Use the synchronous read for simplicity
        //If image or depth is empty raise an error
        if (!color_and_depth.color_image || !color_and_depth.depth_map) {
            throw std::runtime_error("Failed to read color or depth frame from camera: " + name_);
        }
        cv::Mat image(cv::Size(capture_width_, capture_height_), CV_8UC3, (void*)color_and_depth.color_image.get_data(), cv::Mat::AUTO_STEP);
        cv::Mat depth_map(cv::Size(capture_width_, capture_height_), CV_16UC1, (void*)color_and_depth.depth_map.get_data(), cv::Mat::AUTO_STEP);
        int64_t timestamp = color_and_depth.color_image.get_timestamp();
        std::string file_path = "image_" + name_ + "_" + std::to_string(timestamp) + ".jpg";
        {
            std::lock_guard<std::mutex> lock(mtx);
            result = trossen_ai_robot_devices::ImageData{name_, image, depth_map, file_path};
            done = true;
        }
        cv.notify_one();
    }).detach();

    std::unique_lock<std::mutex> lock(mtx);
    if (!cv.wait_for(lock, std::chrono::seconds(5), [&done]{ return done; })) {
        std::cerr << "Timeout: Failed to receive image within 5 seconds." << std::endl;
        // Optionally, set result to an empty image or error state
        result = trossen_ai_robot_devices::ImageData{};
    }
    return result;
}




TrossenAsyncImageWriter::TrossenAsyncImageWriter(int num_threads)
    : stop_flag_(false), num_threads_(num_threads) {
    for (int i = 0; i < num_threads_; ++i) {
        worker_threads_.emplace_back(&TrossenAsyncImageWriter::worker_loop, this);
    }
}

TrossenAsyncImageWriter::~TrossenAsyncImageWriter() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_flag_ = true;
    }
    cv_.notify_all();
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) thread.join();
    }
}

void TrossenAsyncImageWriter::push(const cv::Mat& image, const std::string& filename) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        image_queue_.emplace(image.clone(), filename);  // ensure data safety
    }
    cv_.notify_one();
}

void TrossenAsyncImageWriter::worker_loop() {
    while (true) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this]() {
            return stop_flag_ || !image_queue_.empty();
        });

        if (stop_flag_ && image_queue_.empty())
            break;

        auto [image, filename] = std::move(image_queue_.front());
        image_queue_.pop();
        lock.unlock();

        // Check the channel count to distinguish depth from color
        if (image.channels() == 1) {
            cv::Mat depth_image;
            // If the image is already CV_16UC1, no need to convert
            if (image.type() != CV_16UC1) {
                image.convertTo(depth_image, CV_16UC1, 1000); // Convert depth to 16-bit for saving
            } else {
                depth_image = image;
            }
            try {
                if (!cv::imwrite(filename, depth_image)) {
                    std::cerr << "Failed to write depth image to: " << filename << std::endl;
                }
            } catch (const cv::Exception& e) {
                std::cerr << "Exception while writing depth image to " << filename << ": " << e.what() << std::endl;
            }
        } else {
            // Save color image (assume RGB, convert to BGR for OpenCV)
            cv::Mat bgr_image;
            if (image.channels() == 3) {
                cv::cvtColor(image, bgr_image, cv::COLOR_RGB2BGR);
            } else if (image.channels() == 4) {
                cv::cvtColor(image, bgr_image, cv::COLOR_RGBA2BGR);
            } else {
                // Unexpected, but save as is
                bgr_image = image;
            }
            try {
                if (!cv::imwrite(filename, bgr_image)) {
                    std::cerr << "Failed to write color image to: " << filename << std::endl;
                }
            } catch (const cv::Exception& e) {
                std::cerr << "Exception while writing color image to " << filename << ": " << e.what() << std::endl;
            }
        }
    }
}

} // namespace trossen_data_collection_sdk