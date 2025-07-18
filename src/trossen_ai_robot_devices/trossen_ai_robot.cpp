#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_ai_robot_devices/trossen_ai_driver.hpp"
#include "trossen_dataset/dataset.hpp"
#include <iostream>
#define PI 3.14159265358979323846

namespace trossen_ai_robot_devices {

TrossenAIRobot::TrossenAIRobot(const trossen_sdk_config::RobotConfig& config)
    : name_(config.robot_name) {
    for (const auto& arm_cfg : config.leader_arms) {
        leader_arms_.emplace_back(std::make_unique<TrossenAIArm>(arm_cfg.name, arm_cfg.ip, "leader"));
    }

    for (const auto& arm_cfg : config.follower_arms) {
        follower_arms_.emplace_back(std::make_unique<TrossenAIArm>(arm_cfg.name, arm_cfg.ip, "follower"));
    }

    for (const auto& cam_cfg : config.cameras) {
        cameras_.emplace_back(std::make_unique<TrossenAICamera>(cam_cfg.name, cam_cfg.serial, cam_cfg.width, cam_cfg.height, cam_cfg.fps));
    }
    if (config.base.name == "none") {
        std::cout << "No base configuration provided." << std::endl;
    } else if (config.base.name == "slate") {
        std::cout << "Base config: " << config.base.name << std::endl;
    } else {
        std::cout << "Base configuration: " << config.base.name << std::endl;
    }

    std::cout << "[TrossenAIRobot] Initialized with "
              << leader_arms_.size() << " leader arms, "
              << follower_arms_.size() << " follower arms, "
              << cameras_.size() << " cameras." << std::endl;
}

void TrossenAIRobot::connect() {
    if (is_connected_) {
        std::cout << "Already connected to " << name_ << std::endl;
        return;
    }
    try {
        for (auto& arm : leader_arms_) {
            arm->connect();
        }
        for (auto& arm : follower_arms_) {
            arm->connect();
        }
        for (auto& camera : cameras_) {
            camera->connect();
        }
    } catch (const std::exception& e) {
        std::cerr << "Connection error: " << e.what() << std::endl;
        return;
    }


    is_connected_ = true;
    std::cout << "Connected to Trossen AI Stationary." << std::endl;
}

void TrossenAIRobot::disconnect() {
    std::cout << "Disconnecting from " << name_ << std::endl;
    if (!is_connected_) {
        std::cout << "Not connected to " << name_ << std::endl;
        return;
    }
    for (auto& arm : follower_arms_) {
        arm->disconnect();
    }
    for (auto& camera : cameras_) {
        camera->disconnect();
    }
    is_connected_ = false;
    std::cout << "Disconnected from Trossen AI Stationary." << std::endl;
}


void TrossenAIRobot::teleop_safety_stop() {
    for (auto& arm : leader_arms_) {
        arm->stage_arm(); // Stop leader arms
    }
    for (auto& arm : follower_arms_) {
        arm->stage_arm(); // Stop follower arms
    }
}

trossen_dataset::State TrossenAIRobot::teleop_step(trossen_dataset::EpisodeData& episode_data) {

    // Get the current joint positions from the leader drivers
    std::map<std::string, std::vector<double>> leader_positions_;
    for (auto& arm : leader_arms_) {
        std::vector<double> positions = arm->read("positions");
        leader_positions_[arm->get_name()] = positions;
    }

    for (auto& arm : follower_arms_) {
        // Write the positions to the follower arm
        arm->write("positions", leader_positions_[arm->get_name()]);
    }

    std::map<std::string, std::vector<double>> follower_positions_;
    for (auto& arm : follower_arms_) {
        std::vector<double> positions = arm->read("positions");
        follower_positions_[arm->get_name()] = positions;
    }


    std::vector<double> observation_state;
    // Collect joint positions from both leader and follower arms
    for (auto &state : follower_positions_) {
        observation_state.insert(observation_state.end(), state.second.begin(), state.second.end());
    }
    std::vector<double> action;
    for (auto &state : leader_positions_) {
        action.insert(action.end(), state.second.begin(), state.second.end());
    }
   
    // Collect images from cameras
    std::vector<trossen_dataset::ImageData> images;
    for (auto& camera : cameras_) {
        trossen_dataset::ImageData image_data = camera->async_read();
        images.push_back(image_data);
    }

    trossen_dataset::State state {
        observation_state,
        action,
        images
    };
    return state;

}


const arrow::Status TrossenAIRobot::replay(const std::string& output_file) {
    std::cout << "Replaying joint data from: " << output_file << std::endl;

    std::shared_ptr<arrow::io::ReadableFile> infile;
    // (Doc section: Parquet Read Open)
    // Bind our input file to "test_in.parquet"
    ARROW_ASSIGN_OR_RAISE(infile, arrow::io::ReadableFile::Open(output_file));
    // (Doc section: Parquet Read Open)
    // (Doc section: Parquet FileReader)
    std::unique_ptr<parquet::arrow::FileReader> reader;
    // (Doc section: Parquet FileReader)
    // (Doc section: Parquet OpenFile)
    // Note that Parquet's OpenFile() takes the reader by reference, rather than returning
    // a reader.
    PARQUET_ASSIGN_OR_THROW(reader,
                            parquet::arrow::OpenFile(infile, arrow::default_memory_pool()));
    // (Doc section: Parquet OpenFile)

    // (Doc section: Parquet Read)
    std::shared_ptr<arrow::Table> parquet_table;
    // Read the table.
    PARQUET_THROW_NOT_OK(reader->ReadTable(&parquet_table));

    std::cout << parquet_table->ToString();

 
    // Access columns
    auto timestamp_array = std::static_pointer_cast<arrow::Int64Array>(
        parquet_table->GetColumnByName("timestamp_ms")->chunk(0));
    auto joints_array = std::static_pointer_cast<arrow::ListArray>(
        parquet_table->GetColumnByName("observation.state")->chunk(0));
    auto values_array = std::static_pointer_cast<arrow::DoubleArray>(
        joints_array->values());
    auto action_array = std::static_pointer_cast<arrow::ListArray>(
        parquet_table->GetColumnByName("action")->chunk(0));
    auto episode_idx_array = std::static_pointer_cast<arrow::Int64Array>(
        parquet_table->GetColumnByName("episode_idx")->chunk(0));
    auto frame_idx_array = std::static_pointer_cast<arrow::Int64Array>(
        parquet_table->GetColumnByName("frame_idx")->chunk(0)); 

    for (int64_t i = 0; i < parquet_table->num_rows(); ++i) {
        auto start = joints_array->value_offset(i);
        auto end = joints_array->value_offset(i + 1);
        std::vector<double> joint_positions;
        for (int64_t j = start; j < end; ++j) {
            joint_positions.push_back(values_array->Value(j));
        }
        std::cout << "Timestamp: " << timestamp_array->Value(i)  
                  << ", Episode Index: " << episode_idx_array->Value(i) 
                  << ", Frame Index: " << frame_idx_array->Value(i) << ", Joint Positions: " << std::endl;
        for (const auto& pos : joint_positions) {
            std::cout << pos << " ";
        }
        std::cout << std::endl;
        // Send to the robot arm
        write("positions", joint_positions);
        // Sleep for a short duration to simulate real-time playback
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // Adjust
    }
    std::cout << "Replay completed." << std::endl;
    return arrow::Status::OK();
}


void TrossenAIRobot::write(const std::string& data_name, const std::vector<double>& value) {
        if (value.size() != 14) {
            std::cerr << "Error: Expected 14 joint positions, got " << value.size() << std::endl;
            return;
        }
        // // Set positions for left (first 7) and right (last 7) drivers
        // std::vector<double> left_positions(value.begin(), value.begin() + 7);
        // std::vector<double> right_positions(value.begin() + 7, value.end());

        // follower_left_driver_.write("positions", left_positions);
        // follower_right_driver_.write("positions", right_positions);
}

} // namespace trossen_data_collection_sdk

