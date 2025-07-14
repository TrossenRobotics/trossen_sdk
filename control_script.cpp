#include "trossen_dataset/dataset.hpp"
#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_ai_robot_devices/trossen_ai_cameras.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <boost/program_options.hpp>
#include "trossen_sdk_utils/control_utils.hpp"

namespace po = boost::program_options;

int main(int argc, char* argv[]) {
    std::string dataset_name;
    std::string robot_name;
    int num_episodes;
    double recording_time;

    // Argument parsing
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help,h", "produce help message")
        ("dataset,d", po::value<std::string>(&dataset_name)->default_value("test_dataset_01"), "dataset name")
        ("robot,r", po::value<std::string>(&robot_name)->default_value("Trossen AI Stationary"), "robot name")
        ("episodes,e", po::value<int>(&num_episodes)->default_value(2), "number of episodes")
        ("time,t", po::value<double>(&recording_time)->default_value(10.0), "recording time per episode (seconds)");

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
    trossen_data_collection_sdk::TrossenAsyncImageWriter image_writer(4);

    // Create a dataset instance
    trossen_dataset::TrossenAIDataset dataset(dataset_name);

    trossen_sdk::ControlUtils control_utils;

    // Initialize the robot arm controller
    trossen_data_collection_sdk::TrossenAIStationary robot_controller(robot_name);
    robot_controller.connect(); // Connect to the robot arms

    // Start the control loop for each episode
    for (int episode_idx = 0; episode_idx < num_episodes; ++episode_idx) {
        std::cout << "Starting episode " << episode_idx  << std::endl;
        control_utils.control_loop(robot_controller, recording_time, dataset);
        std::cout << "Episode " << episode_idx << " completed." << std::endl;
    }
    std::cout << "All episodes completed." << std::endl;
    robot_controller.disconnect(); // Sleep the arms at the end of the control script
    std::cout << "Control script finished." << std::endl;
    return 0;
}
