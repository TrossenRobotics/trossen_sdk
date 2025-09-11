#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_sdk_utils/config_parser_utils.hpp"

int main() {
    std::string robot_name = "trossen_ai_stationary"; // Example robot name
    std::string foll_config_file = "../config/" + robot_name + ".json";

    std::ifstream foll_file_check(foll_config_file);
    if (!foll_file_check.good()) {
        spdlog::error("Config file not found for robot: {}", robot_name);
        return 1;
    }
    // Create a robot instance from the configuration
    auto follower_config = trossen_sdk_config::load_follower_config(foll_config_file);
    auto robot_controller = trossen_sdk_config::create_robot_from_config(*follower_config);
    robot_controller->connect(); // Connect to the robot
    robot_controller->disconnect(); // Disconnect from the robot
    return 0;
}
