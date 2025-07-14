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

namespace trossen_data_collection_sdk {


class TrossenAIRobot {
public:
    TrossenAIRobot(const std::string& name)
        : name_(name) {}

    virtual ~TrossenAIRobot() = default;

    virtual void connect() = 0;
    virtual void disconnect() = 0;
    virtual trossen_dataset::State teleop_step(trossen_dataset::EpisodeData& episode_data) = 0;

protected:
    std::string name_;
    std::vector<trossen_ai_robot_devices::TrossenAIArm> leader_arms_;  // Store arms associated with the robot
    std::vector<trossen_ai_robot_devices::TrossenAIArm> follower_arms_;  // Store arms associated with the robot
    std::vector<trossen_data_collection_sdk::TrossenAICamera> cameras_;  // Store cameras associated with the robot
    // Derived classes can define ArmDriverType members as needed
};
class TrossenAIStationary : public TrossenAIRobot {
public:
    TrossenAIStationary(const std::string& name)
        : TrossenAIRobot(name),
          leader_left_driver_("leader_left", "192.168.1.3", "leader"),
          follower_left_driver_("follower_left", "192.168.1.5", "follower"),
          leader_right_driver_("leader_right", "192.168.1.2", "leader"),
          follower_right_driver_("follower_right", "192.168.1.4", "follower"),
          camera_low_("cam_low", "218622274938"),
          camera_high_("cam_high", "130322272628")
    {}

    void connect() override;

    void write(const std::string& data_name, const std::vector<double>& value);

    trossen_dataset::State teleop_step(trossen_dataset::EpisodeData& episode_data) override;

    void disconnect() override;

    void deactivate_leaders() {
        leader_left_driver_.disconnect();
        leader_right_driver_.disconnect();
    }

    void teleop_safety_stop();

    void send_action(const std::vector<double>& action) {
        // Make this modular to handle different action sizes (use metadata or robot configuration)
        if (action.size() != 14) {
            std::cerr << "Error: Expected 14 joint positions, got " << action.size() << std::endl;
            return;
        }
        // Set positions for left (first 7) and right (last 7) drivers
        std::vector<double> left_positions(action.begin(), action.begin() + 7);
        std::vector<double> right_positions(action.begin() + 7, action.end());

        follower_left_driver_.write("positions", left_positions);
        follower_right_driver_.write("positions", right_positions);
    }

    const arrow::Status replay(const std::string& output_file);

    trossen_ai_robot_devices::TrossenAIArm leader_left_driver_;
    trossen_ai_robot_devices::TrossenAIArm follower_left_driver_;
    trossen_ai_robot_devices::TrossenAIArm leader_right_driver_;
    trossen_ai_robot_devices::TrossenAIArm follower_right_driver_;
    trossen_data_collection_sdk::TrossenAICamera camera_low_;
    trossen_data_collection_sdk::TrossenAICamera camera_high_;

private:
    
    bool is_connected_ = false;  // Track connection status

};

} 
