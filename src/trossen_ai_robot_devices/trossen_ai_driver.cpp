#include "trossen_ai_robot_devices/trossen_ai_driver.hpp"
#include <iostream>
#include <algorithm>
#define PI 3.14159265358979323846

namespace trossen_ai_robot_devices {

void TrossenAIArm::connect() {
    if (is_connected_) {
        std::cout << "Already connected to " << name_ << std::endl;
        return;
    }
    try
    {
        if (model_ == "leader") {
        driver_.configure(trossen_arm::Model::wxai_v0, trossen_arm::StandardEndEffector::wxai_v0_leader, ip_address_, true);
    }
    else if (model_ == "follower") {
        driver_.configure(trossen_arm::Model::wxai_v0, trossen_arm::StandardEndEffector::wxai_v0_follower, ip_address_, true);
    } else {
        std::cerr << "Unknown model: " << model_ << std::endl;
        return;
    }
    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
    }
    // Stage the arm
    driver_.set_all_modes(trossen_arm::Mode::position);
    driver_.set_all_positions({0.0, PI/3, PI/6, PI/5, 0.0, 0.0, 0.0}, 2.0, true);

    is_connected_ = true;
}

void TrossenAIArm::disconnect() {
    std::cout << "Disconnecting from " << name_ << std::endl;
    if (!is_connected_) {
        std::cout << "Not connected to " << name_ << std::endl;
        return;
    }
    driver_.set_all_modes(trossen_arm::Mode::position); 
    driver_.set_all_positions({0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}, 2.0, true);
    driver_.set_all_modes(trossen_arm::Mode::idle);
    is_connected_ = false;
}

std::vector<double> TrossenAIArm::read(std::string data_name) {
    if (!is_connected_) {
        std::cerr << "Not connected to " << name_ << ". Cannot read data." << std::endl;
        return {};
    }
    if (data_name == "positions") {
        return driver_.get_all_positions();
    } else if (data_name == "velocities") {
        return driver_.get_all_velocities();
    } else if (data_name == "external_efforts") {
        return driver_.get_all_external_efforts();
    } else {
        std::cerr << "Unknown data name: " << data_name << std::endl;
        return {};
    }
}

void TrossenAIArm::write(const std::string& data_name, const std::vector<double>& data) {
    if (!is_connected_) {
        std::cerr << "Not connected to " << name_ << ". Cannot write data." << std::endl;
        return;
    }
    if (data_name == "positions") {
        // Check if any mode is not set to position
        const auto& modes = driver_.get_modes();
        if (!std::all_of(modes.begin(), modes.end(), [](const auto& mode){ return mode == trossen_arm::Mode::position; })) {
            driver_.set_all_modes(trossen_arm::Mode::position);
        }
        // Check if the data size is correct for positions
        if (data.size() != 7) {
            std::cerr << "Invalid data size for positions. Expected 7 values." << std::endl;
            return;
        }
        driver_.set_all_positions(data, time_to_move_, false);
    } else if (data_name == "velocities") {
        // Check if any mode is not set to velocity
        const auto& modes = driver_.get_modes();
        bool all_velocity = std::all_of(modes.begin(), modes.end(), [](const auto& mode){ return mode == trossen_arm::Mode::velocity; });   
        if (!all_velocity) {
            driver_.set_all_modes(trossen_arm::Mode::velocity);
        }
        // Check if the data size is correct for velocities
        if (data.size() != 7) {
            std::cerr << "Invalid data size for velocities. Expected 7 values." << std::endl;
            return;
        }
        driver_.set_all_velocities(data, time_to_move_, false);
    } else if (data_name == "external_efforts") {
        // Check if any mode is not set to external_effort
        const auto& modes = driver_.get_modes();
        bool all_efforts = std::all_of(modes.begin(), modes.end(), [](const auto& mode){ return mode == trossen_arm::Mode::external_effort; });
        if (!all_efforts) {
            driver_.set_all_modes(trossen_arm::Mode::external_effort);
        }
        // Check if the data size is correct for external efforts
        if (data.size() != 7) {
            std::cerr << "Invalid data size for external_efforts. Expected 7 values." << std::endl;
            return;
        }
        driver_.set_all_external_efforts(data, time_to_move_, false);
    } else {
        std::cerr << "Unknown data name: " << data_name << std::endl;
        return;
    }

}


void TrossenAIArm::stage_arm() {
    if (!is_connected_) {
        std::cerr << "Not connected to " << name_ << ". Cannot stage arm." << std::endl;
        return;
    }
    // Stage the arm to a default position
    driver_.set_all_modes(trossen_arm::Mode::position);
    driver_.set_all_positions({0.0, PI/3, PI/6, PI/5, 0.0, 0.0, 0.0}, 2.0, true);

}

} // namespace trossen_ai_robot_devices