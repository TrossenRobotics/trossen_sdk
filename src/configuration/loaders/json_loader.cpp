#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include <fstream>

nlohmann::json JsonLoader::load(const std::string& path) {
    std::ifstream f(path);
    return nlohmann::json::parse(f);
}
