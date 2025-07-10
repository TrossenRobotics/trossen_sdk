#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_ai_robot_devices/trossen_ai_driver.hpp"
#include "trossen_dataset/dataset.hpp"
#include <iostream>
#define PI 3.14159265358979323846

namespace trossen_data_collection_sdk {



void TrossenAIStationary::connect() {
    if (is_connected_) {
        std::cout << "Already connected to " << name_ << std::endl;
        return;
    }
    try {
        leader_left_driver_.connect();
        follower_left_driver_.connect();
        leader_right_driver_.connect();
        follower_right_driver_.connect();
    } catch (const std::exception& e) {
        std::cerr << "Connection error: " << e.what() << std::endl;
        return;
    }
    is_connected_ = true;
    std::cout << "Connected to Trossen AI Stationary." << std::endl;
}

void TrossenAIStationary::disconnect() {
    std::cout << "Disconnecting from " << name_ << std::endl;
    if (!is_connected_) {
        std::cout << "Not connected to " << name_ << std::endl;
        return;
    }
    leader_left_driver_.disconnect();
    follower_left_driver_.disconnect();
    leader_right_driver_.disconnect();
    follower_right_driver_.disconnect();
    is_connected_ = false;
    std::cout << "Disconnected from Trossen AI Stationary." << std::endl;
}


void TrossenAIStationary::teleop_safety_stop() {
    leader_left_driver_.stage_arm();
    leader_right_driver_.stage_arm();
    follower_left_driver_.stage_arm();
    follower_right_driver_.stage_arm();
}

trossen_dataset::State TrossenAIStationary::teleop_step(trossen_dataset::EpisodeData& episode_data) {

    // Get the current joint positions from the leader drivers
    std::vector<double>  left_leader_positions = leader_left_driver_.read("positions");
    std::vector<double>  right_leader_positions = leader_right_driver_.read("positions");

    // Send the current positions to the follower drivers
    follower_left_driver_.write("positions", left_leader_positions);
    follower_right_driver_.write("positions", right_leader_positions);

    // Get follower left driver positions
    std::vector<double> follower_left_positions = follower_left_driver_.read("positions");
    std::vector<double> follower_right_positions = follower_right_driver_.read("positions");

    // Create observation state with the left follower positions
    std::vector<double> observation_state = follower_left_positions;
    // Append the right follower positions to the observation state
    observation_state.insert(observation_state.end(), follower_right_positions.begin(), follower_right_positions.end());

    // Create action (for now, just a copy of observation state)
    std::vector<double> action = left_leader_positions;
    // Append the right leader positions to the action
    action.insert(action.end(), right_leader_positions.begin(), right_leader_positions.end());

    trossen_dataset::State state {
        observation_state,
        action
    };
    return state;

}


const arrow::Status TrossenAIStationary::replay(const std::string& output_file) {
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


void TrossenAIStationary::write(const std::string& data_name, const std::vector<double>& value) {
        if (value.size() != 14) {
            std::cerr << "Error: Expected 14 joint positions, got " << value.size() << std::endl;
            return;
        }
        // Set positions for left (first 7) and right (last 7) drivers
        std::vector<double> left_positions(value.begin(), value.begin() + 7);
        std::vector<double> right_positions(value.begin() + 7, value.end());

        follower_left_driver_.write("positions", left_positions);
        follower_right_driver_.write("positions", right_positions);
}

} // namespace trossen_data_collection_sdk

