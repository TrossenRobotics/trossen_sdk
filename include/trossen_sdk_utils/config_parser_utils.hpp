#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include <fstream>
#include <vector>
#include <iostream>
#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_sdk_utils/config_utils.hpp"

namespace trossen_sdk_config {

inline trossen_sdk_config::RobotConfig load_robot_config(const std::string& json_path) {
    std::ifstream file(json_path);
    nlohmann::json j;
    file >> j;
    return j.get<trossen_sdk_config::RobotConfig>();
}

inline std::unique_ptr<trossen_ai_robot_devices::TrossenAIRobot> create_robot_from_config(const trossen_sdk_config::RobotConfig& config) {
    // This function can be used to create robot instances based on the configuration
    // For example, you can instantiate TrossenAIStationary or other robot types here
    return std::make_unique<trossen_ai_robot_devices::TrossenAIStationary>(config); // Change the classs name  to something generic
    // Add more robot types as needed
    throw std::runtime_error("Unknown robot type");
}

}  // namespace trossen_sdk_config