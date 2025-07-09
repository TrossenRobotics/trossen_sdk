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


namespace trossen_data_collection_sdk {

class TrossenAIStationary {
public:
    TrossenAIStationary(const std::string& name) : name_(name) {};

    void teleop_step(trossen_dataset::EpisodeData& episode_data);
    void control_loop(int episode_idx, float control_time, const trossen_dataset::Metadata& metadata);
    void sleep_arms();

    void set_arm_pose(const std::vector<double>& positions, double duration = 2.0) {
        if (positions.size() != 14) {
            std::cerr << "Error: Expected 14 joint positions, got " << positions.size() << std::endl;
            return;
        }
        // Set positions for left (first 7) and right (last 7) drivers
        std::vector<double> left_positions(positions.begin(), positions.begin() + 7);
        std::vector<double> right_positions(positions.begin() + 7, positions.end());

        follower_left_driver.set_all_positions(left_positions, duration, false);
        follower_right_driver.set_all_positions(right_positions, duration, false);
    }

    void deactivate_leaders() {
        leader_left_driver.set_all_modes(trossen_arm::Mode::position);
        leader_right_driver.set_all_modes(trossen_arm::Mode::position);
        leader_left_driver.set_all_positions(std::vector<double>(7, 0.0), 2.0, true);
        leader_right_driver.set_all_positions(std::vector<double>(7, 0.0), 2.0, true);
        std::cout << "Deactivating leader arms." << std::endl;
    }

    const arrow::Status replay(const std::string& output_file);

private:
    std::string name_;
    std::map<std::string, trossen_ai_robot_devices::TrossenAIArm> leader_arms_;
    std::map<std::string, trossen_ai_robot_devices::TrossenAIArm> follower_arms_;

};

} 
