#ifndef TROSSEN_SDK_UTILS_CONFIG_UTILS_HPP
#define TROSSEN_SDK_UTILS_CONFIG_UTILS_HPP
#include <string>
#include <nlohmann/json.hpp>
#include <fstream>
#include <vector>
#include <iostream>
#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"


namespace trossen_sdk_config {

/**
 * @brief Camera configuration structure
 * This structure holds the configuration parameters for a camera including its name, serial number,
 * frames per second, resolution, and whether to use depth.
 */
struct CameraConfig {
    std::string name;
    std::string serial;
    int fps;
    int width;
    int height;
    bool use_depth;
};

// TODO [TDS-35] Implement and use Base configuration for mobile base robots
/**
 * @brief Base configuration structure
 * This structure holds the base configuration parameters including the name.
 */
struct BaseConfig {
    std::string name;
};


/**
 * @brief Base class for robot configuration
 * This class serves as a base for specific robot configurations, providing a virtual destructor
 * and a method to get the robot's name.
 */
struct RobotConfigBase {
    virtual ~RobotConfigBase() = default;
    virtual std::string get_name() const = 0;
};

/**
 * @brief Base class for leader robot configuration
 * This class serves as a base for specific leader robot configurations, providing a virtual destructor
 * and a method to get the leader robot's name.
 */
struct LeaderConfigBase {
    virtual ~LeaderConfigBase() = default;
    virtual std::string get_name() const = 0;
};


/**
 * @brief Configuration structure for WidowX robot
 * This structure holds the configuration parameters for a WidowX robot including its name and IP address.
 * It inherits from LeaderConfigBase.
 */
struct WidowXLeaderConfig : public LeaderConfigBase {
    std::string name;
    std::string ip_address;

    std::string get_name() const override {
        return name;
    }
};


/**
 * @brief Configuration structure for Bimanual WidowX robot
 * This structure holds the configuration parameters for a Bimanual WidowX robot including its name,
 * IP addresses for both arms, and camera configurations. It inherits from RobotConfigBase.
 */
struct WidowXRobotConfig : public RobotConfigBase {
    std::string name;
    std::string ip_address;
    std::vector<CameraConfig> cameras;  // Camera configuration for the follower arm
    std::string get_name() const override {
        return name;
    }
};


/**
 * @brief Configuration structure for Bimanual WidowX leader and follower robots
 * This structure holds the configuration parameters for a Bimanual WidowX leader and follower robots
 * including its name, IP addresses for both arms, and camera configurations. It inherits from LeaderConfigBase.
 */
struct BimanualWidowXLeaderConfig : public LeaderConfigBase {
    std::string name;
    std::string left_ip_address;
    std::string right_ip_address;

    std::string get_name() const override {
        return name;
    }
};


/**
 * @brief Configuration structure for Bimanual WidowX robot
 * This structure holds the configuration parameters for a Bimanual WidowX robot including its name,
 * IP addresses for both arms, and camera configurations. It inherits from RobotConfigBase.
 */
struct BimanualWidowXRobotConfig : public RobotConfigBase {
    std::string name;
    std::string left_ip_address;
    std::string right_ip_address;
    std::string get_name() const override {
        return name;
    }
    std::vector<CameraConfig> cameras;  // Camera configuration for the follower arm
};


// JSON serialization/deserialization using nlohmann::json
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CameraConfig, name, serial, fps, width, height, use_depth)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BaseConfig, name)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BimanualWidowXLeaderConfig, name, left_ip_address, right_ip_address)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BimanualWidowXRobotConfig, name, left_ip_address, right_ip_address, cameras)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WidowXLeaderConfig, name, ip_address)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WidowXRobotConfig, name, ip_address, cameras)


}  // namespace trossen_sdk_config
    

#endif // TROSSEN_SDK_UTILS_CONFIG_UTILS_HPP