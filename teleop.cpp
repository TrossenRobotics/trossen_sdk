#include "trossen_dataset/dataset.hpp"
#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_ai_robot_devices/trossen_ai_cameras.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <boost/program_options.hpp>
#include "trossen_sdk_utils/control_utils.hpp"
#include "trossen_sdk_utils/config_parser_utils.hpp"

namespace po = boost::program_options;

int main(int argc, char* argv[]) {

    std::string robot_name;
    double teleop_time;
    double fps;
    bool display_cameras;
    
    // Argument parsing
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "produce help message")
        ("fps", po::value<double>(&fps)->default_value(30.0), "frames per second")
        ("robot", po::value<std::string>(&robot_name)->default_value("trossen_ai_solo"), "robot name")
        ("teleop_time", po::value<double>(&teleop_time)->default_value(10.0), "teleoperation time per episode (seconds)")
        ("display_cameras", po::value<bool>(&display_cameras)->default_value(true), "flag to display camera feeds during recording");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    } catch (const std::exception& ex) {
        spdlog::error("Error parsing arguments: {}", ex.what());
        std::stringstream ss;
        ss << desc;
        spdlog::info("Available options: {}", ss.str());
        return 1;
    }

    if (vm.count("help")) {
        std::stringstream ss;
        ss << desc;
        spdlog::info("{}", ss.str());
        return 0;
    }

    trossen_sdk::ControlUtils control_utils;

    std::string foll_config_file = "../config/" + robot_name + ".json";
    std::string lead_config_file = "../config/" + robot_name + "_leader.json";

    std::ifstream foll_file_check(foll_config_file);
    std::ifstream lead_file_check(lead_config_file);
    if (!foll_file_check.good() || !lead_file_check.good()) {
        spdlog::error("Config file(s) not found for robot: {}", robot_name);
        return 1;
    }
    
    auto follower_config = trossen_sdk_config::load_follower_config(foll_config_file);
    auto robot_controller = trossen_sdk_config::create_robot_from_config(*follower_config);

    auto teleop_config = trossen_sdk_config::load_leader_config(lead_config_file);
    auto teleop_robot = trossen_sdk_config::create_leader_from_config(*teleop_config);

    spdlog::info("Connecting to robot: {}", robot_name);
    robot_controller->connect();
    teleop_robot->connect();

    spdlog::info("Teleoperating the robot arms...");
    control_utils.control_loop(robot_controller, teleop_robot, teleop_time, display_cameras, fps);

    robot_controller->disconnect(); // Sleep the arms at the end of the control script
    teleop_robot->disconnect(); // Disconnect the teleoperation robot
    spdlog::info("Control script finished.");
    return 0;
}
