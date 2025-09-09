#ifndef TROSSEN_AI_CAMERAS_HPP
#define TROSSEN_AI_CAMERAS_HPP
#include <iostream>
#include <string>
#include <librealsense2/rs.hpp>     // RealSense SDK
#include <opencv2/opencv.hpp>       // OpenCV
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <spdlog/spdlog.h>

namespace trossen_ai_robot_devices {

struct ColorDepthData {
    rs2::frame color_image; // Color image captured by the camera
    rs2::frame depth_map;   // Depth map corresponding to the color image
};

struct ImageData {
    std::string camera_name; // Name of the camera that captured the image
    cv::Mat image; // OpenCV Mat to hold the image data
    cv::Mat depth_map; // OpenCV Mat to hold the depth information
};

class TrossenAICamera {
public:
        explicit TrossenAICamera(const std::string& name, const std::string& serial_number, 
                             int capture_width = 640, int capture_height = 480, int fps = 30, bool use_depth = false);

        void connect() ;
        void disconnect() ;
        trossen_ai_robot_devices::ColorDepthData read() ;
        trossen_ai_robot_devices::ImageData async_read();

        const std::string& name() const { return name_; }
        const bool is_using_depth() const { return use_depth_; }
        int width() const { return capture_width_; }
        int height() const { return capture_height_; }
        int fps() const { return fps_; }
        int channels() const { return 3; } // Assuming RGB images


    private:
        std::string name_;
        std::string serial_number_;
        int capture_width_ {640}; // Default width
        int capture_height_ {480}; // Default height
        int fps_ {30}; // Default frames per second
        bool use_depth_ {false}; // Default depth usage
        rs2::pipeline camera_; // RealSense camera pipeline
    };




class TrossenAsyncImageWriter {

public:
    explicit TrossenAsyncImageWriter(int num_threads = 4);
    ~TrossenAsyncImageWriter();

    void push(const cv::Mat& image, const std::string& filename);

private:
    std::queue<std::pair<cv::Mat, std::string>> image_queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<std::thread> worker_threads_;
    std::atomic<bool> stop_flag_;
    int num_threads_;   

    void worker_loop();
};
} // namespace trossen_data_collection_sdk

#endif // TROSSEN_AI_CAMERAS_HPP