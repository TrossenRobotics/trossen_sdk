/**
 * @file json_loader.hpp
 * @brief JSON configuration loader
 */
#ifndef TROSSEN_SDK__CONFIGURATION__LOADERS__JSON_LOADER_HPP_
#define TROSSEN_SDK__CONFIGURATION__LOADERS__JSON_LOADER_HPP_
#include <string>
#include <nlohmann/json.hpp>

class JsonLoader {
public:
    static nlohmann::json load(const std::string& path);
};
#endif  // TROSSEN_SDK__CONFIGURATION__LOADERS__JSON_LOADER_HPP_
