#include "trossen_sdk_utils/control_utils.hpp"

namespace trossen_sdk {

void ControlUtils::control_loop(std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot> robot,
                                std::shared_ptr<trossen_ai_robot_devices::teleoperator::TrossenLeader> teleop_robot,
                                float control_time,
                                trossen_dataset::TrossenAIDataset& dataset,
                                int num_image_writer_threads_per_camera,
                                bool display_cameras,
                                double fps) {
    using steady_clock = std::chrono::steady_clock;
    using time_point = std::chrono::steady_clock::time_point;
                        
    // TODO: Take this from a yaml file / command line argument
    trossen_ai_robot_devices::TrossenAsyncImageWriter image_writer(num_image_writer_threads_per_camera);

    teleop_robot->configure(); // Configure the teleoperation robot
    robot->configure(); // Configure the robot arm
    
    trossen_ai_robot_devices::State state;
    int episode_idx = dataset.get_num_episodes();
    trossen_dataset::EpisodeData episode_data(episode_idx);

    std::string image_folder_path = dataset.get_image_path();
    std::ostringstream oss;
    oss << "episode_" << std::setw(6) << std::setfill('0') << episode_idx;
    std::string episode_folder_name = oss.str();

    std::vector<std::string> camera_names = robot->get_camera_names();
    if (camera_names.empty()) {
        spdlog::error("No cameras found on the robot.");
        return;
    }
    // Create a map from camera name to its folder path
    std::unordered_map<std::string, std::filesystem::path> camera_folder_map;
    std::unordered_map<std::string, std::filesystem::path> depth_camera_folder_map;
    for (const auto& camera_name : camera_names) {
        std::filesystem::path camera_folder = std::filesystem::path(image_folder_path) / camera_name / episode_folder_name;
        std::string depth_camera_name = camera_name + "_depth";
        std::filesystem::path depth_camera_folder = std::filesystem::path(image_folder_path) / depth_camera_name / episode_folder_name;
        if (!std::filesystem::exists(camera_folder)) {
            std::filesystem::create_directories(camera_folder);
        }
        if (!std::filesystem::exists(depth_camera_folder)) {
            std::filesystem::create_directories(depth_camera_folder);
        }
        camera_folder_map[camera_name] = camera_folder;
        depth_camera_folder_map[camera_name] = depth_camera_folder;
    }
    

    auto start_time = steady_clock::now();
    auto end_time = start_time + std::chrono::duration<float>(control_time);
    while (steady_clock::now() < end_time) {
        auto loop_start_time = steady_clock::now();
        trossen_dataset::FrameData frame_data;
        frame_data.timestamp_ms = std::chrono::duration<float>(steady_clock::now() - start_time).count();

        std::vector<double> action = teleop_robot->get_action(); // Get action from the teleoperation robot
        robot->send_action(action); // Send action to the robot arm
        state = robot->get_observation(); // Get the current state from the robot arm
        state.action = action; // Set the action in the state

        
        frame_data.observation_state = state.observation_state;
        frame_data.action = state.action;
        frame_data.episode_idx = episode_idx;
        frame_data.frame_idx = episode_data.get_frames().size();
        episode_data.add_frame(frame_data);
        
        for (const auto& image_data : state.images) {
            const std::string& camera_name = image_data.camera_name;
            std::string filename = image_data.file_path + std::to_string(frame_data.frame_idx) + ".jpg";
            std::string image_file_path = (camera_folder_map[camera_name] / filename).string();

            std::string depth_filename = image_data.file_path + std::to_string(frame_data.frame_idx) + ".jpg";
            std::string depth_image_file_path = (depth_camera_folder_map[camera_name] / depth_filename).string();

            image_writer.push(image_data.image, image_file_path);
            image_writer.push(image_data.depth_map, depth_image_file_path);
        }
        if (display_cameras) {
        // Display images/videos using OpenCV
        display_images(state.images);
        }
        // TODO: Use FPS to control the loop frequency
        busy_wait_until(loop_start_time, fps);  // Use the specified FPS

        auto loop_duration = std::chrono::duration_cast<std::chrono::duration<double>>(steady_clock::now() - loop_start_time).count();
        // TODO: Improve this logging to be more elegant and less verbose
        // TODO: Make FPS tolerance configurable/ constant
        if (loop_duration > 1.0 / fps * 0.85) {
            spdlog::warn("Loop duration: " + std::to_string(loop_duration) + " seconds"
                                    + " | Frequency: " + std::to_string(1.0 / loop_duration) + " Hz"
                                    + " | Episode: " + std::to_string(episode_idx)
                                    + " | Frame: " + std::to_string(frame_data.frame_idx));
        }
        else {
            spdlog::info("Loop duration: " + std::to_string(loop_duration) + " seconds"
                                    + " | Frequency: " + std::to_string(1.0 / loop_duration) + " Hz"
                                    + " | Episode: " + std::to_string(episode_idx)
                                    + " | Frame: " + std::to_string(frame_data.frame_idx));
        }
    }
    
    dataset.save_episode(episode_data);
    
}


void ControlUtils::control_loop(std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot> robot,
                                std::shared_ptr<trossen_ai_robot_devices::teleoperator::TrossenLeader> teleop_robot,
                                float control_time,
                                bool display_cameras,
                                double fps) {
    using steady_clock = std::chrono::steady_clock;
    using time_point = std::chrono::steady_clock::time_point;
                        
    teleop_robot->configure(); // Configure the teleoperation robot
    robot->configure(); // Configure the robot arm
    
    trossen_ai_robot_devices::State state;


    auto start_time = steady_clock::now();
    auto end_time = start_time + std::chrono::duration<float>(control_time);
    while (steady_clock::now() < end_time) {
        auto loop_start_time = steady_clock::now();
        trossen_dataset::FrameData frame_data;
        frame_data.timestamp_ms = std::chrono::duration<float>(steady_clock::now() - start_time).count();

        std::vector<double> action = teleop_robot->get_action(); // Get action from the teleoperation robot
        robot->send_action(action); // Send action to the robot arm
        state = robot->get_observation(); // Get the current state from the robot arm
        state.action = action; // Set the action in the state

        if (display_cameras) {
            // Display images/videos using OpenCV
            display_images(state.images);
        }
        
        // TODO: Use FPS to control the loop frequency
        busy_wait_until(loop_start_time, fps);  // Use the specified FPS

        auto loop_duration = std::chrono::duration_cast<std::chrono::duration<double>>(steady_clock::now() - loop_start_time).count();
        // TODO: Improve this logging to be more elegant and less verbose
        // TODO: Make FPS tolerance configurable/ constant
        if (loop_duration > 1.0 / fps * 0.85) {
            spdlog::warn("Loop duration: " + std::to_string(loop_duration) + " seconds"
                                    + " | Frequency: " + std::to_string(1.0 / loop_duration) + " Hz");
        }
        else {
            spdlog::info("Loop duration: " + std::to_string(loop_duration) + " seconds"
                                    + " | Frequency: " + std::to_string(1.0 / loop_duration) + " Hz");
        }
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