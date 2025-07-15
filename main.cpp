#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_sdk_utils/config_parser_utils.hpp"

int main() {
    // Load the robot configuration
    trossen_sdk_config::RobotConfig robot_config = trossen_sdk_config::load_robot_config("../config/stationary.json");
    // Create a robot instance from the configuration
    auto robot_controller = trossen_sdk_config::create_robot_from_config(robot_config);
    robot_controller->connect(); // Connect to the robot
    robot_controller->disconnect(); // Say hello to the user
    return 0;
}
