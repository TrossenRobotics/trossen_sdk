#pragma once
#include <iostream>
#include <string>
#include <vector>
#include "libtrossen_arm/trossen_arm.hpp"



namespace trossen_ai_robot_devices {

    class TrossenAIArm {
    public:
        TrossenAIArm(const std::string& name, const std::string& ip_address, const std::string& model) : name_(name), ip_address_(ip_address), model_(model) {}
        void connect();
        void disconnect();
        std::vector<double> read(std::string data_name);
        void write(const std::string& data_name, const std::vector<double>& data);

    private:
        std::string name_;
        std::string ip_address_;
        std::string model_;
        trossen_arm::TrossenArmDriver driver_;  // Assuming TrossenArmDriver is the class to control the arm

        bool is_connected_ = false;  // Track connection status
        float time_to_move_ = 0.0;  // Default time to move in seconds
    };

} // namespace trossen_ai_robot_devices


