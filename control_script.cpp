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
    std::string dataset_name;
    std::string robot_name;
    int num_episodes;
    double recording_time;
    double warmup_time;
    double reset_time;
    
    // Argument parsing
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "produce help message")
        ("dataset", po::value<std::string>(&dataset_name)->default_value("test_dataset_01"), "dataset name")
        ("robot", po::value<std::string>(&robot_name)->default_value("trossen_ai_solo"), "robot name")
        ("episodes", po::value<int>(&num_episodes)->default_value(2), "number of episodes")
        ("recording_time", po::value<double>(&recording_time)->default_value(10.0), "recording time per episode (seconds)")
        ("warmup_time", po::value<double>(&warmup_time)->default_value(5.0), "warmup time for the robot arms (seconds)")
        ("reset_time", po::value<double>(&reset_time)->default_value(2.0), "reset time between episodes (seconds)");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    } catch (const std::exception& ex) {
        std::cerr << "Error parsing arguments: " << ex.what() << std::endl;
        std::cout << desc << std::endl;
        return 1;
    }

    if (vm.count("help")) {
        std::cout << desc << std::endl;
        return 0;
    }
    // Initialize the camera
    // Initialize two cameras (camera IDs 0 and 1)
    trossen_ai_robot_devices::TrossenAsyncImageWriter image_writer(4);

   

    trossen_sdk::ControlUtils control_utils;
    std::string config_file;
    if (robot_name == "trossen_ai_stationary") {
        config_file = "../config/stationary.json";
    } else if (robot_name == "trossen_ai_solo") {
        config_file = "../config/solo.json";
    } else if (robot_name == "trossen_ai_mobile") {
        config_file = "../config/mobile.json";
    } else {
        std::cerr << "Unknown robot type: " << robot_name << std::endl;
        return 1;
    }
    std::string foll_config_file = "../config/widowxai.json";
    std::string lead_config_file = "../config/widowx_leader.json";
    // Initialize the robot arm controller
    auto robot_controller = trossen_sdk_config::create_follower_from_config(trossen_sdk_config::load_follower_config(foll_config_file));
    auto teleop_robot = trossen_sdk_config::create_leader_from_config(trossen_sdk_config::load_leader_config(lead_config_file));

    // Create a dataset instance
    std::cout << "Initializing dataset [control_script.cpp]: " << dataset_name << std::endl;
    trossen_dataset::TrossenAIDataset dataset(dataset_name, "test_task", robot_controller);

    // Move the robot controller to this scope
    std::cout << "Connecting to robot: " << std::endl;
    robot_controller->connect(); // Connect to the robot arms
    teleop_robot->connect(); // Connect to the teleoperation robot

    std::cout << "Warming up the robot arms..." << std::endl;
    control_utils.control_loop(robot_controller, teleop_robot, warmup_time, dataset, true); // Warm up the robot arms for specified time

    // Start the control loop for each episode
    for (int episode_idx = 0; episode_idx < num_episodes; ++episode_idx) {
        std::cout << "Starting episode " << dataset.get_num_episodes() << std::endl;
        control_utils.control_loop(robot_controller, teleop_robot, recording_time, dataset);
        std::cout << "Episode " <<  dataset.get_num_episodes() << " completed." << std::endl;
        // Reset time
        std::this_thread::sleep_for(std::chrono::duration<double>(reset_time));
    }
    // dataset.convert_to_videos(dataset.get_image_path());
    // dataset.compute_statistics(); // Compute statistics after all episodes

    robot_controller->disconnect(); // Sleep the arms at the end of the control script
    teleop_robot->disconnect(); // Disconnect the teleoperation robot
    std::cout << "Control script finished." << std::endl;
    return 0;
}
