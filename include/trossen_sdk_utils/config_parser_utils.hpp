#ifndef TROSSEN_SDK_UTILS_CONFIG_PARSER_UTILS_HPP
#define TROSSEN_SDK_UTILS_CONFIG_PARSER_UTILS_HPP
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

inline std::shared_ptr<trossen_ai_robot_devices::TrossenAIRobot> create_robot_from_config(const trossen_sdk_config::RobotConfig& config) {

    return std::make_shared<trossen_ai_robot_devices::TrossenAIRobot>(config);

}

inline trossen_sdk_config::WidowXLeaderConfig load_leader_config(const std::string& json_path) {
    std::ifstream file(json_path);
    nlohmann::json j;
    file >> j;
    return j.get<trossen_sdk_config::WidowXLeaderConfig>();
}

//TODO:: Rename this to load_robot_config after deleting the old load_robot_config function
inline trossen_sdk_config::WidowXRobotConfig load_follower_config(const std::string& json_path) {
    std::ifstream file(json_path);
    nlohmann::json j;
    file >> j;
    return j.get<trossen_sdk_config::WidowXRobotConfig>();  
}

inline std::shared_ptr<trossen_ai_robot_devices::teleoperator::TrossenAIWidowXLeader> create_leader_from_config(const trossen_sdk_config::WidowXLeaderConfig& config) {
    return std::make_shared<trossen_ai_robot_devices::teleoperator::TrossenAIWidowXLeader>(config);

}

inline std::shared_ptr<trossen_ai_robot_devices::robot::TrossenAIWidowXRobot> create_follower_from_config(const trossen_sdk_config::WidowXRobotConfig& config) {
    return std::make_shared<trossen_ai_robot_devices::robot::TrossenAIWidowXRobot>(config);
}

}  // namespace trossen_sdk_config

#endif // TROSSEN_SDK_UTILS_CONFIG_PARSER_UTILS_HPP