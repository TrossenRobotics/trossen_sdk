// robot_config.hpp
#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include <fstream>
#include <vector>
#include <iostream>
#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"


namespace trossen_sdk_config {

struct ArmConfig {
    std::string name;
    std::string ip;
    std::string model;  // e.g. "leader" or "follower"
};

struct CameraConfig {
    std::string name;
    std::string serial;
};

struct RobotConfig {
    std::string robot_name;
    std::vector<ArmConfig> leader_arms;
    std::vector<ArmConfig> follower_arms;
    std::vector<CameraConfig> cameras;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ArmConfig, name, ip, model)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CameraConfig, name, serial)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RobotConfig, robot_name, leader_arms, follower_arms, cameras)


}  // namespace trossen_sdk_config


