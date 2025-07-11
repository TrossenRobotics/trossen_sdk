#include "trossen_ai_robot_devices/trossen_ai_cameras.hpp"

namespace trossen_data_collection_sdk {

TrossenAICamera::TrossenAICamera(const std::string& name, const std::string& serial_number)
    : name_(name), serial_number_(serial_number) {
    // Initialize camera settings if needed
    std::cout << "TrossenAICamera initialized with name: " << name_ 
              << " and serial number: " << serial_number_ << std::endl;
}
void TrossenAICamera::connect() {
    // Connect to the camera
    std::cout << "Connecting to camera: " << name_ << " (Serial: " << serial_number_ << ")" << std::endl;
    // Add connection logic here
    rs2::config cfg;
    if (!serial_number_.empty()) {
        cfg.enable_device(serial_number_);
    }

    cfg.enable_stream(
                rs2::stream::color, capture_width_, capture_height_, rs2::format::rgb8, fps_
            );
    rs2::pipeline camera;
    rs2::pipeline_profile profile = camera.start(cfg);
    // Add more configuration options as needed
    // Start the camera pipeline
    std::cout << "Camera connected successfully." << std::endl;

}

void TrossenAICamera::disconnect() {
    // Disconnect from the camera
    std::cout << "Disconnecting from camera: " << name_ << std::endl;
    // Add disconnection logic here
}

cv::Mat TrossenAICamera::read() {
    // Read a frame from the camera
    std::cout << "Reading frame from camera: " << name_ << std::endl;
    rs2::pipeline camera;
    rs2::frameset frames = camera.wait_for_frames(5000); // Wait for a frame for up to 5000 ms
    rs2::frame color_frame = frames.get_color_frame();
    
    // Convert the frame to OpenCV format
    cv::Mat image(cv::Size(capture_width_, capture_height_), CV_8UC3, 
                  (void*)color_frame.get_data(), cv::Mat::AUTO_STEP);
    
    return image.clone();  // Return a copy of the image
}

trossen_data_collection_sdk::ImageData TrossenAICamera::async_read() {
    // Start an asynchronous read thread
    std::cout << "Starting asynchronous read from camera: " << name_ << std::endl;
    // Note: In a real implementation, you would use a separate thread to read frames asynchronously
    // For simplicity, we will use the synchronous read method here.
    trossen_data_collection_sdk::ImageData result;
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;

    std::thread([this, &result, &mtx, &cv, &done]() {
        cv::Mat image = read();  // Use the synchronous read for simplicity
        std::string file_path = "image_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".jpg";
        {
            std::lock_guard<std::mutex> lock(mtx);
            result = trossen_data_collection_sdk::ImageData{image, file_path};
            done = true;
        }
        cv.notify_one();
    }).detach();

    std::unique_lock<std::mutex> lock(mtx);
    if (!cv.wait_for(lock, std::chrono::seconds(2), [&done]{ return done; })) {
        std::cerr << "Timeout: Failed to receive image within 2 seconds." << std::endl;
        // Optionally, set result to an empty image or error state
        result = trossen_data_collection_sdk::ImageData{};
    }
    return result;
}

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

        cv::imwrite(filename, image);  // Blocking I/O
    }
}

} // namespace trossen_data_collection_sdk