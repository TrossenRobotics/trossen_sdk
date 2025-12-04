#pragma once
#include <string>

struct IConfig {
    virtual ~IConfig() = default;
    virtual std::string type() const = 0;
};
