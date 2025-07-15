#include "trossen_sdk_utils/config_utils.hpp"
#include "trossen_sdk_utils/config_parser_utils.hpp"
#include <iostream>

int main() {
    auto config = trossen_sdk_config::load_robot_config("../config/stationary.json");

    std::cout << "Robot: " << config.robot_name << std::endl;
    for (const auto& arm : config.leader_arms) {
        std::cout << "- Arm: " << arm.name << " (" << arm.model << ") at " << arm.ip << std::endl;
    }
    for (const auto& arm : config.follower_arms) {
        std::cout << "- Arm: " << arm.name << " (" << arm.model << ") at " << arm.ip << std::endl;
    }

    for (const auto& cam : config.cameras) {
        std::cout << "- Camera: " << cam.name << " with serial " << cam.serial << std::endl;
    }

    return 0;
}
