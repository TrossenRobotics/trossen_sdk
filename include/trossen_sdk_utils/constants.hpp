#ifndef TROSSEN_SDK_UTILS_CONSTANTS_HPP
#define TROSSEN_SDK_UTILS_CONSTANTS_HPP 
#include <filesystem>
    
namespace trossen_sdk {

    const std::filesystem::path DEFAULT_ROOT_PATH = std::filesystem::path(std::getenv("HOME")) / ".cache" / "trossen_dataset_collection_sdk";


    const std::string LEADER_ROBOT_CONFIG_FORMAT = "../config/{}_leader.json";
    const std::string FOLLOWER_ROBOT_CONFIG_FORMAT = "../config/{}.json";
} // namespace trossen_sdk

#endif // TROSSEN_SDK_UTILS_CONSTANTS_HPP