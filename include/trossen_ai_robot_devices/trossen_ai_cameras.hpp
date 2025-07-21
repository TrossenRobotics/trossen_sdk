#pragma once

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
namespace trossen_ai_robot_devices {


struct ImageData {
    std::string camera_name; // Name of the camera that captured the image
    cv::Mat image; // OpenCV Mat to hold the image data
    std::string file_path; // File path where the image will be saved
};

class TrossenAICamera {
public:
    
        explicit TrossenAICamera(const std::string& name, const std::string& serial_number, 
                                 int capture_width = 640, int capture_height = 480, int fps = 30);

        void connect() ;
        void disconnect() ;
        rs2::frame read() ;
        trossen_ai_robot_devices::ImageData async_read();

        const std::string& name() const { return name_; }
        int width() const { return capture_width_; }
        int height() const { return capture_height_; }
        int fps() const { return fps_; }
        int channels() const { return 3; } // Assuming RGB images


    private:
        std::string name_;
        std::string serial_number_;
        int capture_width_ = 640; // Default width
        int capture_height_ = 480; // Default height
        int fps_ = 30; // Default frames per second
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