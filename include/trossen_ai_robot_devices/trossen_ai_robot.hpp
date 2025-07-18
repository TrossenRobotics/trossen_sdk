#pragma once
#include <cmath>
#include <iostream>
#include <vector>
#include <string>
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#include <parquet/arrow/reader.h>  // Needed for OpenFile and FileReader
#include <arrow/io/file.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <thread>
#include <chrono>
#include "libtrossen_arm/trossen_arm.hpp"
#include "trossen_dataset/dataset.hpp"
#include "trossen_ai_robot_devices/trossen_ai_driver.hpp"
#include "trossen_ai_robot_devices/trossen_ai_cameras.hpp"
#include "trossen_sdk_utils/config_utils.hpp"

namespace trossen_ai_robot_devices {



class TrossenAIRobot {
public:
    TrossenAIRobot(const trossen_sdk_config::RobotConfig& config);

    void connect();

    void write(const std::string& data_name, const std::vector<double>& value);

    trossen_dataset::State teleop_step(trossen_dataset::EpisodeData& episode_data);

    void disconnect();

    void deactivate_leaders() {
        for (auto& arm : leader_arms_) {
            arm->disconnect(); // Stop leader arms
        }
    }

    void teleop_safety_stop();

    void send_action(const std::vector<double>& action) {
        // Make this modular to handle different action sizes (use metadata or robot configuration)
        // if (action.size() != 14) {
        //     std::cerr << "Error: Expected 14 joint positions, got " << action.size() << std::endl;
        //     return;
        // }
        // // Set positions for left (first 7) and right (last 7) drivers
        // std::vector<double> left_positions(action.begin(), action.begin() + 7);
        // std::vector<double> right_positions(action.begin() + 7, action.end());

        // follower_left_driver_->write("positions", left_positions);
        // follower_right_driver_->write("positions", right_positions);
    }

    const arrow::Status replay(const std::string& output_file);
    std::vector<std::unique_ptr<trossen_ai_robot_devices::TrossenAIArm>> leader_arms_;
    std::vector<std::unique_ptr<trossen_ai_robot_devices::TrossenAIArm>> follower_arms_;
    std::vector<std::unique_ptr<trossen_ai_robot_devices::TrossenAICamera>> cameras_;

private:
    std::string name_;
    bool is_connected_ = false;  // Track connection status
   
};



} // namespace trossen_ai_robot_devices