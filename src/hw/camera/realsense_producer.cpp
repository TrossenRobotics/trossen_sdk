// This is a testfile to create a script to test the realsense camera producer.
// This is to verify that the realsense sdk can be integrated properly from the CmakeLists.txt
// and that the realsense camera producer can be compiled without errors.

#include <iostream>
#include <librealsense2/rs.hpp>
#include <opencv2/opencv.hpp>
#include <filesystem>

int main() {
    std::cout << "Starting RealSense camera test..." << std::endl;

    try {
        // Create a context to manage devices
        rs2::context ctx;
        auto devices = ctx.query_devices();

        if (devices.size() == 0) {
            std::cout << "No RealSense cameras found." << std::endl;
            return -1;
        }

        // Get the first available device
        auto dev = devices[0];
        std::string serial = dev.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
        std::string name = dev.get_info(RS2_CAMERA_INFO_NAME);

        std::cout << "Found camera: " << name << " with Serial Number: " << serial << std::endl;

        // Create pipeline and config
        rs2::pipeline pipe;
        rs2::config cfg;

        // Configure the pipeline
        cfg.enable_device(serial);
        cfg.enable_stream(RS2_STREAM_COLOR, 640, 480, RS2_FORMAT_RGB8, 30);

        std::cout << "Connecting to camera..." << std::endl;

        // Start the camera
        rs2::pipeline_profile profile = pipe.start(cfg);

        std::cout << "Camera connected successfully!" << std::endl;

        // Let the camera stabilize
        std::cout << "Waiting for camera to stabilize..." << std::endl;
        for (int i = 0; i < 10; i++) {
            rs2::frameset frames = pipe.wait_for_frames();
        }

        std::cout << "Taking a picture..." << std::endl;

        // Capture a frame
        rs2::frameset frames = pipe.wait_for_frames();
        rs2::frame color_frame = frames.get_color_frame();

        if (color_frame) {
            // Convert to OpenCV Mat
            const void* data_ptr = static_cast<const void*>(color_frame.get_data());
            cv::Mat image(cv::Size(640, 480), CV_8UC3,
                    const_cast<void*>(data_ptr), cv::Mat::AUTO_STEP);

            // Create output directory if it doesn't exist
            std::filesystem::create_directories("outputs/realsense_test");

            // Save the image
            std::string filename = "outputs/realsense_test/test_image_" + serial + ".png";

            // Convert from RGB to BGR for OpenCV
            cv::Mat bgr_image;
            cv::cvtColor(image, bgr_image, cv::COLOR_RGB2BGR);

            bool saved = cv::imwrite(filename, bgr_image);

            if (saved) {
                std::cout << "Image saved successfully to: " << filename << std::endl;
            } else {
                std::cout << "Failed to save image" << std::endl;
            }
        } else {
            std::cout << "Failed to capture color frame" << std::endl;
        }

        std::cout << "Disconnecting from camera..." << std::endl;

        // Stop the pipeline
        pipe.stop();

        std::cout << "Camera disconnected successfully!" << std::endl;
        std::cout << "RealSense test completed successfully!" << std::endl;
    } catch (const rs2::error& e) {
        std::cout << "RealSense error: " << e.what() << std::endl;
        return -1;
    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << std::endl;
        return -1;
    }

    return 0;
}
