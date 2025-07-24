#ifndef TROSSEN_SDK_UTILS_LOGGING_UTILS_HPP
#define TROSSEN_SDK_UTILS_LOGGING_UTILS_HPP

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


    //TODO: Move these into a cpp file
    inline void log_info(const std::string& message) {
        std::cout << "[INFO] " << message << std::endl;
    }

    inline void log_warning(const std::string& message) {
        std::cout << YELLOW << "[WARN] " << message << RESET << std::endl;
    }

    inline void log_error(const std::string& message) {
        std::cerr << RED << "[ERRO] " << message << RESET << std::endl;
    }
}  // namespace trossen_sdk_utils


#endif // TROSSEN_SDK_UTILS_LOGGING_UTILS_HPP