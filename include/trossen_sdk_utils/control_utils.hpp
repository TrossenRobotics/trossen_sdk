#pragma once
#include <string>
#include <vector>
#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_sdk_utils/logging_utils.hpp"
#include "trossen_ai_robot_devices/trossen_ai_cameras.hpp"
#include <filesystem>
namespace trossen_sdk {

class ControlUtils {
public:
    // Function to write joint positions to the robot arm
    void control_loop(trossen_ai_robot_devices::TrossenAIRobot* robot, 
    float control_time,
    trossen_dataset::TrossenAIDataset& dataset,
    bool teleop_mode = false);

    inline void busy_wait_until(const std::chrono::steady_clock::time_point& loop_start, double frequency) {
    using namespace std::chrono;
    auto desired_duration = duration<double>(1.0 / frequency);
    auto elapsed = steady_clock::now() - loop_start;
    auto remaining = duration_cast<microseconds>(desired_duration - elapsed);
    if (remaining.count() > 0){
        std::this_thread::sleep_for(remaining);
    }
    }

    void display_images(const std::vector<trossen_dataset::ImageData>& images) const;
    
private:
    // Logging utility
};

} // namespace trossen_sdk