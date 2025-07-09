#include "trossen_dataset/dataset.hpp"
#include "trossen_data_collection_sdk/arms_move.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <boost/program_options.hpp>

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

    std::cout << "Control script started." << std::endl;
    
    // Initialize metadata
    trossen_dataset::Metadata metadata(dataset_name);
    metadata.add_entry("robot_name", robot_name);
    metadata.add_entry("num_episodes", std::to_string(num_episodes));
    metadata.add_entry("recording_time", std::to_string(recording_time));   

    // Initialize the robot arm controller
    trossen_data_collection_sdk::TrossenAIStationary robot_controller(robot_name);

    // Start the control loop for each episode
    for (int episode_idx = 0; episode_idx < num_episodes; ++episode_idx) {
        std::cout << "Starting episode " << episode_idx + 1 << " of " << num_episodes << std::endl;
        robot_controller.control_loop(episode_idx, recording_time, metadata);
        std::cout << "Episode " << episode_idx + 1 << " completed." << std::endl;
    }       
    std::cout << "All episodes completed." << std::endl;
    robot_controller.sleep_arms(); // Sleep the arms at the end of the control script
    std::cout << "Control script finished." << std::endl;
    return 0;
}
