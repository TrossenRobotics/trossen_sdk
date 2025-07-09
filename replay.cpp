#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_dataset/dataset.hpp"
#include <boost/program_options.hpp>
#include <filesystem>

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
    trossen_data_collection_sdk::TrossenAIStationary robot(robot_name);
    robot.connect(); // Connect to the robot arms
    // robot.deactivate_leaders(); // Deactivate the leader arms
    std::filesystem::path dataset_path = std::filesystem::path(std::getenv("HOME")) / ".cache" / "trossen_dataset_collection_sdk" / dataset_name / "data" / ("episode_" + std::to_string(episode_number) + ".parquet");
    robot.replay(dataset_path.string());
    robot.disconnect();

    return 0;
}
