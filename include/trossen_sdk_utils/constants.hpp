#ifndef TROSSEN_SDK_UTILS_CONSTANTS_HPP
#define TROSSEN_SDK_UTILS_CONSTANTS_HPP 
#include <filesystem>
    
namespace trossen_sdk {

    // Default root path for dataset storage
    const std::filesystem::path DEFAULT_ROOT_PATH = std::filesystem::path(std::getenv("HOME")) / ".cache" / "trossen_dataset_collection_sdk";

    // Configuration file path formats
    const std::string LEADER_ROBOT_CONFIG_FORMAT = "../config/{}_leader.json";
    const std::string FOLLOWER_ROBOT_CONFIG_FORMAT = "../config/{}.json";


    // Read and write data names
    const std::string POSITION = "position";
    const std::string VELOCITY = "velocity";
    const std::string EXTERNAL_EFFORT = "external_effort";


    // Robot models
    const std::string LEADER_MODEL = "leader";
    const std::string FOLLOWER_MODEL = "follower";
} // namespace trossen_sdk

#endif // TROSSEN_SDK_UTILS_CONSTANTS_HPP