/**
* @file json_loader.cpp
* @brief JSON configuration loader implementation
*/

#include <fstream>

#include "trossen_sdk/configuration/loaders/json_loader.hpp"

nlohmann::json trossen::configuration::JsonLoader::load(const std::string& path) {
    std::ifstream f(path);
    return nlohmann::json::parse(f);
}
