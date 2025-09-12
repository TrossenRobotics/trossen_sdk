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
    std::string repo_id;
    std::string root;
    int fps;

    po::options_description desc("Allowed options");
    desc.add_options()
        ("help,h", "produce help message")
        ("dataset,d", po::value<std::string>(&dataset_name)->required(), "dataset name")
        ("robot,r", po::value<std::string>(&robot_name)->required(), "robot name")
        ("episode,e", po::value<int>(&episode_number)->required(), "episode number")
        ("repo,r", po::value<std::string>(&repo_id)->required(), "repository id")
        ("root,R", po::value<std::string>(&root)->required(), "root directory")
        ("fps,f", po::value<int>(&fps)->default_value(30), "frames per second");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::stringstream ss;
            ss << desc;
            spdlog::info("{}", ss.str());
            exit(0);
        }

        po::notify(vm);
    } catch (const po::error &e) {
        spdlog::error("Error: {}", e.what());
        std::stringstream ss;
        ss << desc;
        spdlog::info("{}", ss.str());
        exit(1);
    }

    // Load robot configurations
    std::string foll_config_file = fmt::format(trossen_sdk::FOLLOWER_ROBOT_CONFIG_FORMAT, robot_name);
    
    std::ifstream foll_file_check(foll_config_file);
    if (!foll_file_check.good()) {
        spdlog::error("Config file not found for robot: {}", robot_name);
        return 1;
    }
    // Create a robot instance from the configuration
    auto follower_config = trossen_sdk_config::load_follower_config(foll_config_file);
    auto robot_controller = trossen_sdk_config::create_robot_from_config(*follower_config);

    std::filesystem::path root_path = root.empty() ? trossen_sdk::DEFAULT_ROOT_PATH : std::filesystem::path(root);

    robot_controller->connect(); // Connect to the robot arms
    trossen_dataset::TrossenAIDataset dataset(dataset_name,"replay_task", robot_controller, root_path, repo_id, false, false, 0, fps);
    std::vector<std::vector<double>> joints_array = dataset.read(episode_number);
    if (joints_array.empty()) {
        spdlog::error("No joint data found in the specified episode.");
        return 1;
    }
    std::vector<double> joint_positions;
    // Loop start time
    trossen_sdk::ControlUtils control_utils;
    for (int64_t i = 0; i < joints_array.size(); ++i) {
        auto loop_start_time = std::chrono::steady_clock::now();
        joint_positions = joints_array[i];
        robot_controller->send_action(joint_positions);
        control_utils.busy_wait_until(loop_start_time , static_cast<double>(fps));
    }
    spdlog::info("Replay completed.");
    robot_controller->disconnect();

    return 0;
}
