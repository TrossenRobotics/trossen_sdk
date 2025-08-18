#ifndef TROSSEN_AI_DRIVER_HPP
#define TROSSEN_AI_DRIVER_HPP
#include <iostream>
#include <string>
#include <vector>
#include "libtrossen_arm/trossen_arm.hpp"
#include <spdlog/spdlog.h>



namespace trossen_ai_robot_devices {

    class TrossenAIArm {
    public:
        TrossenAIArm(const std::string& name, const std::string& ip_address, const std::string& model) : name_(name), ip_address_(ip_address), model_(model) {}
        
        // Move constructor and assignment
        TrossenAIArm(TrossenAIArm&&) noexcept = default;
        TrossenAIArm& operator=(TrossenAIArm&&) noexcept = default;

        // Delete copy constructor and assignment
        TrossenAIArm(const TrossenAIArm&) = delete;
        TrossenAIArm& operator=(const TrossenAIArm&) = delete;

        void connect();
        void disconnect();
        std::vector<double> read(std::string data_name);
        void write(const std::string& data_name, const std::vector<double>& data);
        void stage_arm();
        std::string get_name() const { return name_; }

    private:
        std::string name_;
        std::string ip_address_;
        std::string model_;
        trossen_arm::TrossenArmDriver driver_;  // Assuming TrossenArmDriver is the class to control the arm

        bool is_connected_ = false;  // Track connection status
        float time_to_move_ = 0.1;  // Default time to move in seconds
    };

} // namespace trossen_ai_robot_devices


#endif // TROSSEN_AI_DRIVER_HPP