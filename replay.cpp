#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_sdk_utils/config_parser_utils.hpp"
#include "trossen_dataset/dataset.hpp"
#include <boost/program_options.hpp>
#include <filesystem>
#include <trossen_sdk_utils/control_utils.hpp>

namespace po = boost::program_options;


int main(int argc, char* argv[]) {
    std::string dataset_name;
    std::string robot_name;
    int episode_number;

    po::options_description desc("Allowed options");
    desc.add_options()
        ("help,h", "produce help message")
        ("dataset,d", po::value<std::string>(&dataset_name)->required(), "dataset name")
        ("robot,r", po::value<std::string>(&robot_name)->required(), "robot name")
        ("episode,e", po::value<int>(&episode_number)->required(), "episode number");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << desc << "\n";
            exit(0);
        }

        po::notify(vm);
    } catch (const po::error &e) {
        std::cerr << "Error: " << e.what() << "\n";
        std::cout << desc << "\n";
        exit(1);
    }

    // Load the robot configuration
    std::string foll_config_file = "../config/bimanual_widowxai.json";
    // Create a robot instance from the configuration
    auto robot_controller = trossen_sdk_config::create_follower_from_config(trossen_sdk_config::load_bimanual_follower_config(foll_config_file));

    robot_controller->connect(); // Connect to the robot arms
    std::filesystem::path dataset_path = std::filesystem::path(std::getenv("HOME")) / ".cache" / "trossen_dataset_collection_sdk" / dataset_name / "data" / ("episode_" + std::to_string(episode_number) + ".parquet");
    std::vector<std::vector<double>> joints_array = trossen_dataset::TrossenAIDataset::read(dataset_path.string());
    if (joints_array.empty()) {
        std::cerr << "No joint data found in the specified episode." << std::endl;
        return 1;
    }
    std::vector<double> joint_positions;

    // Loop start time
    trossen_sdk::ControlUtils control_utils;
    for (int64_t i = 0; i < joints_array.size(); ++i) {
        auto loop_start_time = std::chrono::steady_clock::now();
        joint_positions = joints_array[i];
        std::cout << "Replaying frame " << i << " with joint positions: ";
        for (const auto& pos : joint_positions) {
            std::cout << pos << " ";
        }
        std::cout << std::endl;
        robot_controller->send_action(joint_positions);
        control_utils.busy_wait_until(loop_start_time , 30.0);
    }
    

    std::this_thread::sleep_for(std::chrono::milliseconds(1));  // Adjust
    std::cout << "Replay completed." << std::endl;
    robot_controller->disconnect();

    return 0;
}
