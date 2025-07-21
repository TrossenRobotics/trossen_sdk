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
#include "trossen_ai_robot_devices/trossen_ai_driver.hpp"
#include "trossen_ai_robot_devices/trossen_ai_cameras.hpp"
#include "trossen_sdk_utils/config_utils.hpp"
#include <any>

namespace trossen_ai_robot_devices {


struct State{
    std::vector<double> observation_state;  // Joint positions
    std::vector<double> action;             // Action to be taken
    std::vector<ImageData> images; // Images captured during the episode
};

class TrossenAIRobot {
public:
    TrossenAIRobot(const trossen_sdk_config::RobotConfig& config);

    void connect();

    void write(const std::string& data_name, const std::vector<double>& value);

    trossen_ai_robot_devices::State teleop_step();

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

    const std::string& name() const { return name_; }

    std::map<std::string, std::map<std::string, std::any>> camera_features() const {
        std::map<std::string, std::map<std::string, std::any>> cam_ft;
        for (const auto& cam_ptr : cameras_) {
            if (!cam_ptr) continue;
            std::string cam_key = cam_ptr->name();
            std::string key = "observation.images." + cam_key;
            cam_ft[key] = {
                {"shape", std::make_tuple(cam_ptr->height(), cam_ptr->width(), cam_ptr->channels())},
                {"names", std::vector<std::string>{"height", "width", "channels"}},
                {"info", nullptr}
            };
        }
        return cam_ft;
    }

private:
    std::string name_;
    bool is_connected_ = false;  // Track connection status
   
};



} // namespace trossen_ai_robot_devices