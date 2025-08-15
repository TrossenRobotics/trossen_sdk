#ifndef TROSSEN_SDK_UTILS_CONFIG_UTILS_HPP
#define TROSSEN_SDK_UTILS_CONFIG_UTILS_HPP
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
    int fps;
    int width;
    int height;
};

struct BaseConfig {
    std::string name;
};
struct RobotConfig {
    std::string robot_name;
    std::vector<ArmConfig> leader_arms;
    std::vector<ArmConfig> follower_arms;
    std::vector<CameraConfig> cameras;
    BaseConfig base;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ArmConfig, name, ip, model)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CameraConfig, name, serial, fps, width, height)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BaseConfig, name)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RobotConfig, robot_name, leader_arms, follower_arms, cameras, base)


struct WidowXLeaderConfig {
    std::string name;
    std::string ip_address;
};
struct WidowXRobotConfig {
    std::string name;
    std::string ip_address;
    CameraConfig camera;  // Camera configuration for the follower arm

};

struct BimanualWidowXLeaderConfig {
    std::string name;
    std::string left_ip_address;
    std::string right_ip_address;
};

struct BimanualWidowXRobotConfig {
    std::string name;
    std::string left_ip_address;
    std::string right_ip_address;
    CameraConfig camera;  // Camera configuration for the follower arm
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BimanualWidowXLeaderConfig, name, left_ip_address, right_ip_address)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BimanualWidowXRobotConfig, name, left_ip_address, right_ip_address, camera)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WidowXLeaderConfig, name, ip_address)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WidowXRobotConfig, name, ip_address, camera)

}  // namespace trossen_sdk_config
    

#endif // TROSSEN_SDK_UTILS_CONFIG_UTILS_HPP