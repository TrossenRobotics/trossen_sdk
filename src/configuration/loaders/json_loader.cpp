/**
* @file json_loader.cpp
* @brief JSON configuration loader implementation
*/
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include <fstream>

nlohmann::json trossen::configuration::JsonLoader::load(const std::string& path) {
    std::ifstream f(path);
    return nlohmann::json::parse(f);
}
