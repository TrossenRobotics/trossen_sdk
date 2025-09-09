#ifndef TROSSEN_SDK_UTILS_CONFIG_UTILS_HPP
#define TROSSEN_SDK_UTILS_CONFIG_UTILS_HPP
#include <string>
#include <nlohmann/json.hpp>
#include <fstream>
#include <vector>
#include <iostream>
#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"


namespace trossen_sdk_config {

struct CameraConfig {
    std::string name;
    std::string serial;
    int fps;
    int width;
    int height;
    bool use_depth;
};

struct BaseConfig {
    std::string name;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CameraConfig, name, serial, fps, width, height, use_depth)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BaseConfig, name)


struct RobotConfigBase {
    virtual ~RobotConfigBase() = default;
    virtual std::string get_name() const = 0;
};

struct LeaderConfigBase {
    virtual ~LeaderConfigBase() = default;
    virtual std::string get_name() const = 0;
};

struct WidowXLeaderConfig : public LeaderConfigBase {
    std::string name;
    std::string ip_address;

    std::string get_name() const override {
        return name;
    }
};

struct WidowXRobotConfig : public RobotConfigBase {
    std::string name;
    std::string ip_address;
    std::vector<CameraConfig> cameras;  // Camera configuration for the follower arm
    std::string get_name() const override {
        return name;
    }
};

struct BimanualWidowXLeaderConfig : public LeaderConfigBase {
    std::string name;
    std::string left_ip_address;
    std::string right_ip_address;

    std::string get_name() const override {
        return name;
    }
};

struct BimanualWidowXRobotConfig : public RobotConfigBase {
    std::string name;
    std::string left_ip_address;
    std::string right_ip_address;
    std::string get_name() const override {
        return name;
    }
    std::vector<CameraConfig> cameras;  // Camera configuration for the follower arm
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BimanualWidowXLeaderConfig, name, left_ip_address, right_ip_address)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BimanualWidowXRobotConfig, name, left_ip_address, right_ip_address, cameras)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WidowXLeaderConfig, name, ip_address)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WidowXRobotConfig, name, ip_address, cameras)

}  // namespace trossen_sdk_config
    

#endif // TROSSEN_SDK_UTILS_CONFIG_UTILS_HPP