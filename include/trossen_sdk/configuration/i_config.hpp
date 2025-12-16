/**
 * @file i_config.hpp
 * @brief Interface for configuration types
 */
#ifndef TROSSEN_SDK__CONFIGURATION__I_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__I_CONFIG_HPP_
#include <string>

struct BaseConfig {
    virtual ~BaseConfig() = default;
    virtual std::string type() const = 0;
};
#endif  // TROSSEN_SDK__CONFIGURATION__I_CONFIG_HPP_
