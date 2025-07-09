#pragma once

#include <iostream>
#include <vector>
#include <string>

#define RED "\033[31m"
#define GREEN "\033[32m"
#define YELLOW "\033[33m"
#define BLUE "\033[34m"
#define MAGENTA "\033[35m"
#define RESET "\033[0m"
namespace trossen_sdk_utils {


    //TODo: Move these into a cpp file
    inline void log_info(const std::string& message) {
        std::cout << BLUE << "[INFO] " << message << RESET << std::endl;
    }

    inline void log_warning(const std::string& message) {
        std::cout << YELLOW << "[WARNING] " << message << RESET << std::endl;
    }

    inline void log_error(const std::string& message) {
        std::cerr << RED << "[ERROR] " << message << RESET << std::endl;
    }
}  // namespace trossen_sdk_utils
