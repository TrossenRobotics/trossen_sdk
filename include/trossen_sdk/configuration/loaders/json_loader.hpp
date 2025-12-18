#pragma once
#include <string>
#include <nlohmann/json.hpp>

class JsonLoader {
public:
    static nlohmann::json load(const std::string& path);
};
