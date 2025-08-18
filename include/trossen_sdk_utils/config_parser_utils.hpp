#ifndef TROSSEN_SDK_UTILS_CONFIG_PARSER_UTILS_HPP
#define TROSSEN_SDK_UTILS_CONFIG_PARSER_UTILS_HPP
#include <string>
#include <nlohmann/json.hpp>
#include <fstream>
#include <vector>
#include <iostream>
#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_sdk_utils/config_utils.hpp"
#include <spdlog/spdlog.h>

namespace trossen_sdk_config {

inline std::shared_ptr<trossen_sdk_config::LeaderConfigBase> load_leader_config(const std::string& json_path) {
    std::ifstream file(json_path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + json_path);
    }

    nlohmann::json j;
    file >> j;

    // You must add "type": "widowx" or "bimanual" in your JSON
    const std::string& type = j.at("type");

    if (type == "widowx") {
        return std::make_shared<trossen_sdk_config::WidowXLeaderConfig>(j.get<trossen_sdk_config::WidowXLeaderConfig>());
    } else if (type == "bimanual") {
        return std::make_shared<trossen_sdk_config::BimanualWidowXLeaderConfig>(j.get<trossen_sdk_config::BimanualWidowXLeaderConfig>());
    } else {
        throw std::runtime_error("Unknown leader config type: " + type);
    }
}
//TODO:: Rename this to load_robot_config after deleting the old load_robot_config function
inline std::shared_ptr<trossen_sdk_config::RobotConfigBase> load_follower_config(const std::string& json_path) {
    std::ifstream file(json_path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + json_path);
    }

    nlohmann::json j;
    file >> j;

    // You must add "type": "widowx" or "bimanual" in your JSON
    const std::string& type = j.at("type");

    if (type == "widowx") {
        return std::make_shared<trossen_sdk_config::WidowXRobotConfig>(j.get<trossen_sdk_config::WidowXRobotConfig>());
    } else if (type == "bimanual") {
        return std::make_shared<trossen_sdk_config::BimanualWidowXRobotConfig>(j.get<trossen_sdk_config::BimanualWidowXRobotConfig>());
    } else {
        throw std::runtime_error("Unknown follower config type: " + type);
    }
}

inline std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot> create_robot_from_config(const trossen_sdk_config::RobotConfigBase& config) {
    if (config.get_name() == "widowxai") {
        auto leader_config = dynamic_cast<const trossen_sdk_config::WidowXRobotConfig&>(config);
        return std::make_shared<trossen_ai_robot_devices::robot::TrossenAIWidowXRobot>(leader_config);
    } else if (config.get_name() == "bimanual_widowxai") {
        auto robot_config = dynamic_cast<const trossen_sdk_config::BimanualWidowXRobotConfig&>(config);
        return std::make_shared<trossen_ai_robot_devices::robot::TrossenAIBimanualWidowXRobot>(robot_config);
    } else {
        throw std::runtime_error("Unknown robot type in configuration");
    }
}

inline std::shared_ptr<trossen_ai_robot_devices::teleoperator::TrossenLeader> create_leader_from_config(const trossen_sdk_config::LeaderConfigBase& config) {
    if (config.get_name() == "widowxai_leader") {
        auto leader_config = dynamic_cast<const trossen_sdk_config::WidowXLeaderConfig&>(config);
        return std::make_shared<trossen_ai_robot_devices::teleoperator::TrossenAIWidowXLeader>(leader_config);
    } else if (config.get_name() == "bimanual_widowxai_leader") {
        auto leader_config = dynamic_cast<const trossen_sdk_config::BimanualWidowXLeaderConfig&>(config);
        return std::make_shared<trossen_ai_robot_devices::teleoperator::TrossenAIBimanualWidowXLeader>(leader_config);
    } else {
        throw std::runtime_error("Unknown leader type in configuration");
    }
}
}  // namespace trossen_sdk_config

#endif // TROSSEN_SDK_UTILS_CONFIG_PARSER_UTILS_HPP