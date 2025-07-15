#include "trossen_ai_robot_devices/trossen_ai_cameras.hpp"


int main() {

    // Initialize two cameras (camera IDs 0 and 1)
    trossen_ai_robot_devices::TrossenAsyncImageWriter image_writer(4);

    trossen_ai_robot_devices::TrossenAICamera camera0("cam_low", "218622274938");
    trossen_ai_robot_devices::TrossenAICamera camera1("cam_high", "130322272628");
    camera0.connect();
    camera1.connect();

    // Start Time
    auto start_time = std::chrono::steady_clock::now();
    float control_time = 1.0f; // Control time in seconds

    while(std::chrono::duration<float>(std::chrono::steady_clock::now() - start_time).count() < control_time) {
        // Read images from both cameras
        trossen_dataset::ImageData image0 = camera0.async_read();
        trossen_dataset::ImageData image1 = camera1.async_read();

        image_writer.push(image0.image, image0.file_path);
        image_writer.push(image1.image, image1.file_path);
    }


    return 0;
}
