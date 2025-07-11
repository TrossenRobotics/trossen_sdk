#include "trossen_sdk_utils/control_utils.hpp"
#include "trossen_dataset/dataset.hpp"

namespace trossen_sdk {

void ControlUtils::control_loop(trossen_data_collection_sdk::TrossenAIStationary& robot, float control_time, trossen_dataset::TrossenAIDataset& dataset) {

    // Start a asynchronous image writer (TODO: Move this to a better location)
    trossen_data_collection_sdk::TrossenAsyncImageWriter image_writer(4);

    //TODO: Move this logic to a separate function
    robot.leader_left_driver_.write("external_efforts", std::vector<double>(7, 0.0));
    robot.leader_right_driver_.write("external_efforts", std::vector<double>(7, 0.0));

    auto start_time = std::chrono::system_clock::now();
    auto end_time = start_time + std::chrono::seconds(static_cast<int>(control_time));

    // State to hold the observation and action
    trossen_dataset::State state;

    int episode_idx = dataset.get_num_episodes();  // Get the current episode index
    // Create EpisodeData for logging
    trossen_dataset::EpisodeData episode_data(episode_idx);

    while (std::chrono::system_clock::now() < end_time) {
        // Get the start time of the loop
        auto loop_start_time = std::chrono::system_clock::now();

        state = robot.teleop_step(episode_data);  // Call the teleop step function

        // Create a FrameData object to log the current state
        trossen_dataset::FrameData frame_data;
        frame_data.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        frame_data.observation_state = state.observation_state;  // Joint positions
        frame_data.action = state.action;  // Action to be taken
        frame_data.episode_idx = episode_idx;  // Episode index
        frame_data.frame_idx = episode_data.get_frames().size();  // Frame index
        episode_data.add_frame(frame_data);  // Add the frame data to the episode
        // Get the file location from metadata in the dataset
        std::string image_folder_path = dataset.get_image_path();
        // Save images from the cameras
        for (const auto& image_data : state.images) {
            // add the above folder
            std::string image_file_path = (std::filesystem::path(image_folder_path) / image_data.file_path).string();
            // Save the image using the async image writer
            image_writer.push(image_data.image, image_file_path);
        }   


        // Get the total time taken for the loop
        auto loop_end_time = std::chrono::system_clock::now();
        auto loop_duration = std::chrono::duration_cast<std::chrono::duration<double>>(loop_end_time - loop_start_time).count();

        trossen_sdk_utils::log_info("Loop duration: " + std::to_string(loop_duration) + " seconds" 
                                    + " | Frequency: " + std::to_string(1.0 / loop_duration) + " Hz"
                                    + " | Episode: " + std::to_string(episode_idx) 
                                    + " | Frame: " + std::to_string(frame_data.frame_idx));
        
    }
    dataset.save_episode(episode_data);  // Close the episode data to finalize it
    std::cout << "Control loop finished." << std::endl;
    robot.teleop_safety_stop();
    

}


} // namespace trossen_sdk