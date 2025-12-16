#pragma once
#include <string>

struct BaseConfig {
    virtual ~BaseConfig() = default;
    virtual std::string type() const = 0;
};
