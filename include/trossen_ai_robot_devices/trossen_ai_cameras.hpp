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


/// @brief Data structure to hold color and depth raw frames from realsense camera
struct ColorDepthData {
    rs2::frame color_image; // Color image captured by the camera
    rs2::frame depth_map;   // Depth map corresponding to the color image
};

/// @brief Data structure to hold image data from the camera including depth map if available in OpenCV Mat format
struct ImageData {
    std::string camera_name; // Name of the camera that captured the image
    cv::Mat image; // OpenCV Mat to hold the image data
    cv::Mat depth_map; // OpenCV Mat to hold the depth information
};

class TrossenAICamera {
public:
        /**
         * @brief Constructor for TrossenAICamera
         * @param name Name of the camera
         * @param serial_number Serial number of the camera
         * @param capture_width Width of the captured images
         * @param capture_height Height of the captured images
         * @param fps Frames per second for image capture
         * @param use_depth Flag to indicate if depth map should be captured
         */
        explicit TrossenAICamera(const std::string& name, const std::string& serial_number, 
                             int capture_width = 640, int capture_height = 480, int fps = 30, bool use_depth = false);
        
        /**
         * @brief Connect to the RealSense camera and start the pipeline
         * Enables color and depth streams based on configuration
         * Starts the camera pipeline
         */
        void connect() ;

        /**
         * @brief Disconnect from the camera
         * Logs that the camera is being disconnected
         */
        void disconnect() ;

        /**
         * @brief Read a frame from the camera
         * Waits for a new frameset and extracts color and depth frames
         * @return ColorDepthData structure containing color image and depth map frames
         */
        trossen_ai_robot_devices::ColorDepthData read() ;

        /**
         * @brief Asynchronously read a frame from the camera
         * @return ImageData structure containing color image and depth map
         */
        trossen_ai_robot_devices::ImageData async_read();
        
        // Getters

        /// @brief Get the name of the camera
        const std::string& name() const { return name_; }

        /// @brief Check if depth map is being used
        const bool is_using_depth() const { return use_depth_; }

        /// @brief Get the width of the captured images
        int width() const { return capture_width_; }

        /// @brief Get the height of the captured images
        int height() const { return capture_height_; }

        /// @brief Get the frames per second for image capture
        int fps() const { return fps_; }

        //TODO [TDS-31] Define channels based on image format
        /// @brief Get the number of channels in the captured images
        int channels() const { return 3; } // Assuming RGB images


    private:
        std::string name_; // Name of the camera
        std::string serial_number_; // Serial number of the camera
        int capture_width_ {640}; // Default width
        int capture_height_ {480}; // Default height
        int fps_ {30}; // Default frames per second
        bool use_depth_ {false}; // Default depth usage
        rs2::pipeline camera_; // RealSense camera pipeline
    };




class TrossenAsyncImageWriter {

public:
    /**
     * @brief Constructor for TrossenAsyncImageWriter
     * @param num_threads Number of threads to use for writing images
     * Initializes worker threads to process image writing tasks
     */
    explicit TrossenAsyncImageWriter(int num_threads = 4);

    /**
     * @brief Destructor for TrossenAsyncImageWriter
     * Signals all worker threads to stop and joins them
     */
    ~TrossenAsyncImageWriter();

    /**
     * @brief Push an image to be written asynchronously
     * @param image OpenCV Mat containing the image data
     * @param filename Path where the image should be saved
     * Adds the image and filename to the queue for asynchronous writing
     */
    void push(const cv::Mat& image, const std::string& filename);

private:
    std::queue<std::pair<cv::Mat, std::string>> image_queue_; // Queue to hold images and their filenames
    std::mutex mtx_; // Mutex for thread-safe access to the queue
    std::condition_variable cv_; // Condition variable for notifying worker threads
    std::vector<std::thread> worker_threads_; // Vector to hold worker threads
    std::atomic<bool> stop_flag_; // Flag to signal worker threads to stop
    int num_threads_; // Number of worker threads

    /**
     * @brief Worker loop for processing image writing tasks
     * Continuously checks the queue for new images to write
     * Exits when stop_flag_ is set to true and the queue is empty
     */
    void worker_loop();
};
} // namespace trossen_data_collection_sdk

#endif // TROSSEN_AI_CAMERAS_HPP