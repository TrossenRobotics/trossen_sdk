#ifndef TROSSEN_SDK_UTILS_CONFIG_PARSER_UTILS_HPP
#define TROSSEN_SDK_UTILS_CONFIG_PARSER_UTILS_HPP
#include <spdlog/spdlog.h>

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_sdk_utils/config_utils.hpp"

namespace trossen_sdk_config {

/**
 * @brief Load leader robot configuration from a JSON file
 * @param json_path Path to the JSON configuration file
 * @return Shared pointer to the loaded LeaderConfigBase object
 * This function reads the JSON file, determines the type of leader robot configuration,
 * and deserializes it into the appropriate derived class of LeaderConfigBase.
 */
inline std::shared_ptr<trossen_sdk_config::LeaderConfigBase> load_leader_config(
    const std::string& json_path) {
    // Check if file exists
    if (!std::filesystem::exists(json_path)) {
        throw std::runtime_error("Configuration file does not exist: " + json_path);
    }
    // Read the JSON file
    std::ifstream file(json_path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + json_path);
    }

    // Parse the JSON content
    nlohmann::json j;
    file >> j;

    // Check if "type" exists in the JSON
    if (!j.contains("type")) {
        throw std::runtime_error("Missing 'type' field in leader config JSON: " + json_path);
    }
    // Extract the type and create the appropriate config object
    const std::string& type = j.at("type");

    if (type == "widowx") {
        return std::make_shared<trossen_sdk_config::WidowXLeaderConfig>(
            j.get<trossen_sdk_config::WidowXLeaderConfig>());
    } else if (type == "bimanual") {
        return std::make_shared<trossen_sdk_config::BimanualWidowXLeaderConfig>(
            j.get<trossen_sdk_config::BimanualWidowXLeaderConfig>());
    } else {
        throw std::runtime_error("Unknown leader config type: " + type);
    }
}

/**
 * @brief Load follower robot configuration from a JSON file
 * @param json_path Path to the JSON configuration file
 * @return Shared pointer to the loaded RobotConfigBase object
 * This function reads the JSON file, determines the type of follower robot configuration,
 * and deserializes it into the appropriate derived class of RobotConfigBase.
 */
inline std::shared_ptr<trossen_sdk_config::RobotConfigBase> load_robot_config(
    const std::string& json_path) {
    // Check if file exists
    if (!std::filesystem::exists(json_path)) {
        throw std::runtime_error("Configuration file does not exist: " + json_path);
    }

    // Read the JSON file
    std::ifstream file(json_path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + json_path);
    }

    // Parse the JSON content
    nlohmann::json j;
    file >> j;

    // Check if "type" exists in the JSON
    if (!j.contains("type")) {
        throw std::runtime_error("Missing 'type' field in leader config JSON: " + json_path);
    }
    // Extract the type and create the appropriate config object
    const std::string& type = j.at("type");

    if (type == "widowx") {
        return std::make_shared<trossen_sdk_config::WidowXRobotConfig>(
            j.get<trossen_sdk_config::WidowXRobotConfig>());
    } else if (type == "bimanual") {
        return std::make_shared<trossen_sdk_config::BimanualWidowXRobotConfig>(
            j.get<trossen_sdk_config::BimanualWidowXRobotConfig>());
    } else {
        throw std::runtime_error("Unknown follower config type: " + type);
    }
}

/**
 * @brief Create a TrossenRobot instance from a RobotConfigBase object
 * @param config Reference to a RobotConfigBase object
 * @return Shared pointer to the created TrossenRobot instance
 * This function creates an instance of the appropriate TrossenRobot derived class
 * based on the type of the provided RobotConfigBase object.
 */
inline std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot> create_robot_from_config(
    const trossen_sdk_config::RobotConfigBase& config) {
    // Determine the type of robot and create the appropriate instance
    if (config.get_name() == "widowxai") {
        auto leader_config = dynamic_cast<const trossen_sdk_config::WidowXRobotConfig&>(config);
        return std::make_shared<trossen_ai_robot_devices::robot::TrossenAIWidowXRobot>(
            leader_config);
    } else if (config.get_name() == "bimanual_widowxai") {
        auto robot_config =
            dynamic_cast<const trossen_sdk_config::BimanualWidowXRobotConfig&>(config);
        return std::make_shared<trossen_ai_robot_devices::robot::TrossenAIBimanualWidowXRobot>(
            robot_config);
    } else {
        throw std::runtime_error("Unknown robot type in configuration: " + config.get_name());
    }
}

/**
 * @brief Create a TrossenLeader instance from a LeaderConfigBase object
 * @param config Reference to a LeaderConfigBase object
 * @return Shared pointer to the created TrossenLeader instance
 * This function creates an instance of the appropriate TrossenLeader derived class
 * based on the type of the provided LeaderConfigBase object.
 */
inline std::shared_ptr<trossen_ai_robot_devices::teleoperator::TrossenLeader>
create_leader_from_config(const trossen_sdk_config::LeaderConfigBase& config) {
    // Determine the type of leader and create the appropriate instance
    if (config.get_name() == "widowxai_leader") {
        auto leader_config = dynamic_cast<const trossen_sdk_config::WidowXLeaderConfig&>(config);
        return std::make_shared<trossen_ai_robot_devices::teleoperator::TrossenAIWidowXLeader>(
            leader_config);
    } else if (config.get_name() == "bimanual_widowxai_leader") {
        auto leader_config =
            dynamic_cast<const trossen_sdk_config::BimanualWidowXLeaderConfig&>(config);
        return std::make_shared<
            trossen_ai_robot_devices::teleoperator::TrossenAIBimanualWidowXLeader>(leader_config);
    } else {
        throw std::runtime_error("Unknown leader type in configuration: " + config.get_name());
    }
}
}  // namespace trossen_sdk_config

#endif  // TROSSEN_SDK_UTILS_CONFIG_PARSER_UTILS_HPP