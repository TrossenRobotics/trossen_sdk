#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"

int main() {
    trossen_data_collection_sdk::TrossenAIStationary robot("Trossen AI Stationary");
    robot.connect(); // Connect to the robot
    robot.disconnect(); // Say hello to the user
    return 0;
}
