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
namespace trossen_data_collection_sdk {



struct ImageData {
    cv::Mat image; // OpenCV Mat to hold the image data
    std::string file_path; // File path where the image will be saved
};

class TrossenAICamera {
public:
    
    explicit TrossenAICamera(const std::string& name, const std::string& serial_number = "")
            : name_(name), serial_number_(serial_number) {}

        void connect() ;
        void disconnect() ;
        cv::Mat read() ;
        trossen_data_collection_sdk::ImageData async_read();


    private:
        std::string name_;
        std::string serial_number_;
        int capture_width_ = 640; // Default width
        int capture_height_ = 480; // Default height
        int fps_ = 30; // Default frames per second
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