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
    void control_loop(trossen_data_collection_sdk::TrossenAIStationary& robot, 
    float control_time,
    trossen_dataset::TrossenAIDataset& dataset);

private:
    // Logging utility
};

} // namespace trossen_sdk