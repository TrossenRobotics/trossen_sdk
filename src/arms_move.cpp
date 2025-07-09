#include "trossen_data_collection_sdk/arms_move.hpp"
#include "trossen_ai_robot_devices/trossen_ai_driver.hpp"
#include "trossen_dataset/dataset.hpp"
#include <iostream>

namespace trossen_data_collection_sdk {

TrossenAIStationary::TrossenAIStationary(const std::string& name)
    : name_(name),
      leader_left_driver("leader_left", "192.168.1.2", "leader"),
      follower_left_driver("follower_left", "192.168.1.5", "follower"),
      leader_right_driver("leader_right", "192.168.1.3", "leader"),
      follower_right_driver("follower_right", "192.168.1.4", "follower")
{
    // Initialize the arm driver maps with pointers to member variables
    leader_arms_["leader_left"] = &leader_left_driver;
    follower_arms_["follower_left"] = &follower_left_driver;
    leader_arms_["leader_right"] = &leader_right_driver;
    follower_arms_["follower_right"] = &follower_right_driver;
}

void TrossenAIStationary::control_loop(int episode_idx, float control_time, const trossen_dataset::Metadata& metadata) {

    leader_arms_["leader_left"]->set_all_modes(trossen_arm::Mode::external_effort);
    follower_arms_["follower_left"]->set_all_modes(trossen_arm::Mode::position);

    leader_arms_["leader_left"]->set_all_external_efforts(std::vector<double>(7, 0.0), 0.0, true);

    auto start_time = std::chrono::system_clock::now();
    auto end_time = start_time + std::chrono::seconds(static_cast<int>(control_time));

    // Create EpisodeData for logging
    trossen_dataset::EpisodeData episode_data(episode_idx, metadata);  // Assuming a single episode

    while (std::chrono::system_clock::now() < end_time) {
        teleop_step(episode_data);  // Call the teleop step function
    }
    episode_data.close();  // Close the episode data to finalize it
    std::cout << "Control loop finished." << std::endl;

}


void TrossenAIStationary::teleop_step(trossen_dataset::EpisodeData& episode_data) {

    // Get the current joint positions
    std::vector<double>  left_current_positions = leader_left_driver.get_all_positions();
    std::vector<double>  right_current_positions = leader_right_driver.get_all_positions();

    // send the current positions to the leader left driver
    follower_left_driver.set_all_positions(left_current_positions, 0.0, false);
    follower_right_driver.set_all_positions(right_current_positions, 0.0, false);

    // Get follower left driver positions
    std::vector<double> follower_left_positions = follower_left_driver.get_all_positions();
    std::vector<double> follower_right_positions = follower_right_driver.get_all_positions();

    // Create observation state
    std::vector<double> observation_state = follower_left_positions;
    observation_state.insert(observation_state.end(), follower_right_positions.begin(), follower_right_positions.end());

    // Create action (for now, just a copy of observation state)
    std::vector<double> action = left_current_positions;
    

    // Create a FrameData sample
    trossen_dataset::FrameData frame_data;
    frame_data.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    frame_data.observation_state = observation_state;
    frame_data.action = action;
    frame_data.episode_idx = episode_data.get_episode_idx();  // Assuming a single episode for now
    frame_data.frame_idx = episode_data.get_frames().size() + 1;     // Assuming a single frame for now

    episode_data.add_frame(frame_data);  // logger_ is std::unique_ptr<JointLogger>

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
        set_arm_pose(joint_positions, 0.0);
        // Sleep for a short duration to simulate real-time playback
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // Adjust
    }
    std::cout << "Replay completed." << std::endl;
    return arrow::Status::OK();
}

} // namespace trossen_data_collection_sdk

