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
    /// @brief Name of the camera
    std::string name;
    /// @brief Serial number of the camera
    std::string serial;
    /// @brief Frames per second for image capture
    int fps;
    /// @brief Width of the camera images
    int width;
    /// @brief Height of the camera images
    int height;
    /// @brief Whether to use depth information
    bool use_depth;
};

// TODO [TDS-35] Implement and use Base configuration for mobile base robots
/**
 * @brief Base configuration structure
 * This structure holds the base configuration parameters including the name.
 */
struct BaseConfig {
    /// @brief Name of the configuration
    std::string name;
};


/**
 * @brief Base class for robot configuration
 * This class serves as a base for specific robot configurations, providing a virtual destructor
 * and a method to get the robot's name.
 */
struct RobotConfigBase {
    /**
     * @brief Virtual destructor
     */
    virtual ~RobotConfigBase() = default;
    /**
     * @brief Get the name of the robot
     * @return Name of the robot as a string
     */
    virtual std::string get_name() const = 0;
};

/**
 * @brief Base class for leader robot configuration
 * This class serves as a base for specific leader robot configurations, providing a virtual destructor
 * and a method to get the leader robot's name.
 */
struct LeaderConfigBase {

    /** @brief Virtual destructor
     *  This is a virtual destructor for the leader configuration base class.
    */
    virtual ~LeaderConfigBase() = default;

    /** @brief Get the name of the leader robot
     * @return Name of the leader robot as a string
    */
    virtual std::string get_name() const = 0;
};


/**
 * @brief Configuration structure for WidowX robot
 * This structure holds the configuration parameters for a WidowX robot including its name and IP address.
 * It inherits from LeaderConfigBase.
 */
struct WidowXLeaderConfig : public LeaderConfigBase {
    /// @brief Name of the leader robot
    std::string name;
    /// @brief IP address of the leader robot
    std::string ip_address;

    /** @brief Get the name of the leader robot 
     * @return Name of the leader robot as a string
    */
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
    /// @brief Name of the robot
    std::string name;
    /// @brief IP address of the robot
    std::string ip_address;
    /// @brief Camera configurations for the robot
    std::vector<CameraConfig> cameras;  // Camera configuration for the follower arm

    /** @brief Get the name of the robot
     * @return Name of the robot as a string
     */
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
    /// @brief Name of the bimanual robot leader
    std::string name;
    /// @brief IP address of the left arm
    std::string left_ip_address;
    /// @brief IP address of the right arm
    std::string right_ip_address;

    /**
     * @brief Get the name of the bimanual robot leader
     * @return Name of the bimanual robot leader as a string
     */
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
    /// @brief Name of the bimanual robot
    std::string name;
    /// @brief IP address of the left arm
    std::string left_ip_address;
    /// @brief IP address of the right arm
    std::string right_ip_address;

    /** @brief Get the name of the bimanual robot
     * @return Name of the bimanual robot as a string
     */
    std::string get_name() const override {
        return name;
    }

    /// @brief Camera configurations for the bimanual robot
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