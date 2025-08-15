#include "trossen_sdk_utils/control_utils.hpp"

namespace trossen_sdk {

void ControlUtils::control_loop(std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot> robot,
                                std::shared_ptr<trossen_ai_robot_devices::teleoperator::TrossenLeader> teleop_robot,
                                float control_time,
                                trossen_dataset::TrossenAIDataset& dataset,
                                bool teleop_mode) {
    using steady_clock = std::chrono::steady_clock;
    using time_point = std::chrono::steady_clock::time_point;
                        
    // TODO: Take this from a yaml file / command line argument
    trossen_ai_robot_devices::TrossenAsyncImageWriter image_writer(4);

    teleop_robot->configure(); // Configure the teleoperation robot
    robot->configure(); // Configure the robot arm

    auto start_time = steady_clock::now();
    auto end_time = start_time + std::chrono::duration<float>(control_time);
    
    trossen_ai_robot_devices::State state;
    int episode_idx = dataset.get_num_episodes();
    trossen_dataset::EpisodeData episode_data(episode_idx);

    while (steady_clock::now() < end_time) {
        auto loop_start_time = steady_clock::now();

        std::vector<double> action = teleop_robot->get_action(); // Get action from the teleoperation robot
        robot->send_action(action); // Send action to the robot arm
        state = robot->get_observation(); // Get the current state from the robot arm
        state.action = action; // Set the action in the state

        trossen_dataset::FrameData frame_data;
        frame_data.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            steady_clock::now().time_since_epoch()).count();
        frame_data.observation_state = state.observation_state;
        frame_data.action = state.action;
        frame_data.episode_idx = episode_idx;
        frame_data.frame_idx = episode_data.get_frames().size();
        episode_data.add_frame(frame_data);
        if(!teleop_mode) {
        
            std::string image_folder_path = dataset.get_image_path();

            for (const auto& image_data : state.images) {
                std::string episode_folder_name = "episode_" + std::to_string(episode_idx);
                std::filesystem::path camera_folder = std::filesystem::path(image_folder_path) / image_data.camera_name / episode_folder_name;
                std::string depth_camera_name = image_data.camera_name + "_depth";
                std::filesystem::path depth_camera_folder = std::filesystem::path(image_folder_path) / depth_camera_name / episode_folder_name;
                if (!std::filesystem::exists(camera_folder)) {
                    std::filesystem::create_directories(camera_folder);
                }
                if (!std::filesystem::exists(depth_camera_folder)) {
                    std::filesystem::create_directories(depth_camera_folder);
                }
                std::string image_file_path = (camera_folder / image_data.file_path).string();
                std::string depth_image_file_path = (depth_camera_folder / image_data.file_path).string();

                image_writer.push(image_data.image, image_file_path);
                image_writer.push(image_data.depth_map, depth_image_file_path);
            }
        }
        // Display images/videos using OpenCV
        display_images(state.images);
        // TODO: Use FPS to control the loop frequency
        busy_wait_until(loop_start_time, 30.0);  // 30 Hz loop 

        auto loop_duration = std::chrono::duration_cast<std::chrono::duration<double>>(steady_clock::now() - loop_start_time).count();
        // TODO: Improve this logging to be more elegant and less verbose
        // TODO: Make FPS tolerance configurable/ constant
        if (loop_duration > 1.0 / 29.0) {
            trossen_sdk_utils::log_warning("Loop duration: " + std::to_string(loop_duration) + " seconds"
                                    + " | Frequency: " + std::to_string(1.0 / loop_duration) + " Hz"
                                    + " | Episode: " + std::to_string(episode_idx)
                                    + " | Frame: " + std::to_string(frame_data.frame_idx));
        }
        else {
            trossen_sdk_utils::log_info("Loop duration: " + std::to_string(loop_duration) + " seconds"
                                    + " | Frequency: " + std::to_string(1.0 / loop_duration) + " Hz"
                                    + " | Episode: " + std::to_string(episode_idx)
                                    + " | Frame: " + std::to_string(frame_data.frame_idx));
        }
    }
    if (!teleop_mode) {
         dataset.save_episode(episode_data);
    }   
}


void ControlUtils::display_images(const std::vector<trossen_ai_robot_devices::ImageData>& images) const {
    // Display all images together in a grid format using OpenCV
        if (!images.empty()) {
            // Determine grid size (rows x cols)
            int num_images = images.size();
            int grid_cols = static_cast<int>(std::ceil(std::sqrt(num_images)));
            int grid_rows = static_cast<int>(std::ceil(static_cast<float>(num_images) / grid_cols));

            // Resize all images to the same size (use the first image's size)
            cv::Size target_size = images[0].image.size();
            std::vector<cv::Mat> resized_images;
            for (const auto& image_data : images) {
            cv::Mat resized;
            cv::resize(image_data.image, resized, target_size);
            resized_images.push_back(resized);
            }

            // Create a blank canvas for the grid
            int grid_width = grid_cols * target_size.width;
            int grid_height = grid_rows * target_size.height;
            cv::Mat grid_image(grid_height, grid_width, resized_images[0].type(), cv::Scalar::all(0));

            // Copy each image into the grid
            for (int idx = 0; idx < num_images; ++idx) {
            int row = idx / grid_cols;
            int col = idx % grid_cols;
            cv::Rect roi(col * target_size.width, row * target_size.height, target_size.width, target_size.height);
            cv::Mat rgb_image;
            cv::cvtColor(resized_images[idx], rgb_image, cv::COLOR_BGR2RGB);
            rgb_image.copyTo(grid_image(roi));
            }

            cv::imshow("Camera Grid", grid_image);
            cv::waitKey(1);
        }
}


} // namespace trossen_sdk