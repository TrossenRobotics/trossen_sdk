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


namespace trossen_data_collection_sdk {

class TrossenAIStationary {
public:
    TrossenAIStationary(const std::string& name)
        : name_(name),
          leader_left_driver_("leader_left", "192.168.1.3", "leader"),
          follower_left_driver_("follower_left", "192.168.1.5", "follower"),
          leader_right_driver_("leader_right", "192.168.1.2", "leader"),
          follower_right_driver_("follower_right", "192.168.1.4", "follower")
    {}

    void connect();
    void write(const std::string& data_name, const std::vector<double>& value);
    trossen_dataset::FrameData teleop_step(trossen_dataset::EpisodeData& episode_data);
    void control_loop(int episode_idx, float control_time, const trossen_dataset::Metadata& metadata);
    void disconnect();

    void deactivate_leaders() {
        leader_left_driver_.disconnect();
        leader_right_driver_.disconnect();
        std::cout << "Deactivating leader arms." << std::endl;
    }

    const arrow::Status replay(const std::string& output_file);

private:
    std::string name_;
    trossen_ai_robot_devices::TrossenAIArm leader_left_driver_;
    trossen_ai_robot_devices::TrossenAIArm follower_left_driver_;
    trossen_ai_robot_devices::TrossenAIArm leader_right_driver_;
    trossen_ai_robot_devices::TrossenAIArm follower_right_driver_;
    bool is_connected_ = false;  // Track connection status

};

} 
