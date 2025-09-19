// Copyright 2025 Trossen Robotics

#include "trossen_sdk_utils/control_utils.hpp"

namespace trossen_sdk {

void ControlUtils::control_loop(
    std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot> robot,
    std::shared_ptr<trossen_ai_robot_devices::teleoperator::TrossenLeader>
        teleop_robot,
    float control_time, trossen_dataset::TrossenAIDataset* dataset,
    bool display_cameras, double fps) {
  using steady_clock = std::chrono::steady_clock;
  using time_point = std::chrono::steady_clock::time_point;

  teleop_robot->configure();  // Configure the teleoperation robot
  robot->configure();         // Configure the robot arm

  trossen_ai_robot_devices::State state;

  auto start_time = steady_clock::now();
  auto end_time = start_time + std::chrono::duration<float>(control_time);
  while (steady_clock::now() < end_time) {
    auto loop_start_time = steady_clock::now();
    trossen_dataset::FrameData frame_data;
    // Get action from the teleoperation robot and send to the robot arm
    std::vector<double> action = teleop_robot->get_action();
    robot->send_action(action);

    // Get the current state from the robot arm
    robot->get_observation(&state);

    // Set the action in the state
    state.action = action;

    frame_data.observation_state = state.observation_state;
    frame_data.action = state.action;
    for (auto& image_data : state.images) {
      frame_data.images.push_back(image_data);
    }
    // Add frame to the dataset
    dataset->add_frame(&frame_data);

    if (display_cameras) {
      // Display images/videos using OpenCV
      display_images(state.images);
    }

    // Clear images to save memory
    state.images.clear();

    busy_wait_until(loop_start_time, fps);  // Use the specified FPS

    auto loop_duration =
        std::chrono::duration_cast<std::chrono::duration<double>>(
            steady_clock::now() - loop_start_time)
            .count();
    // TODO(shantanuparab-tr): Improve this logging to be more elegant and less
    // verbose
    // TODO(shantanuparab-tr): Make FPS tolerance configurable/ constant
    if (loop_duration > 1.0 / fps * 0.85) {
      spdlog::warn("Loop duration: " + std::to_string(loop_duration) +
                   " seconds" +
                   " | Frequency: " + std::to_string(1.0 / loop_duration) +
                   " Hz" + " | Frame: " + std::to_string(frame_data.frame_idx));
    } else {
      spdlog::info("Loop duration: " + std::to_string(loop_duration) +
                   " seconds" +
                   " | Frequency: " + std::to_string(1.0 / loop_duration) +
                   " Hz" + " | Frame: " + std::to_string(frame_data.frame_idx));
    }
  }

  dataset->save_episode();
}

void ControlUtils::control_loop(
    std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot> robot,
    std::shared_ptr<trossen_ai_robot_devices::teleoperator::TrossenLeader>
        teleop_robot,
    float control_time, bool display_cameras, double fps) {
  using steady_clock = std::chrono::steady_clock;
  using time_point = std::chrono::steady_clock::time_point;

  teleop_robot->configure();  // Configure the teleoperation robot
  robot->configure();         // Configure the robot arm

  trossen_ai_robot_devices::State state;

  auto start_time = steady_clock::now();
  auto end_time = start_time + std::chrono::duration<float>(control_time);
  while (steady_clock::now() < end_time) {
    auto loop_start_time = steady_clock::now();
    trossen_dataset::FrameData frame_data;
    frame_data.timestamp_s =
        std::chrono::duration<float>(steady_clock::now() - start_time).count();

    // Get action from the teleoperation robot and send to the robot arm
    std::vector<double> action = teleop_robot->get_action();
    robot->send_action(action);
    // Get the current state from the robot arm
    robot->get_observation(&state);
    // Set the action in the state
    state.action = action;

    // Display images/videos using OpenCV if enabled
    if (display_cameras) {
      display_images(state.images);
    }

    // Clear images to save memory
    state.images.clear();

    // Wait until the next loop iteration based on the desired frequency
    busy_wait_until(loop_start_time, fps);

    // Check the loop duration and log warnings if it exceeds the expected time
    auto loop_duration =
        std::chrono::duration_cast<std::chrono::duration<double>>(
            steady_clock::now() - loop_start_time)
            .count();
    // TODO(shantanuparab-tr): Improve this logging to be more elegant and less
    // verbose
    // TODO(shantanuparab-tr): Make FPS tolerance configurable/ constant
    if (loop_duration > 1.0 / fps * 0.85) {
      spdlog::warn(
          "Loop duration: " + std::to_string(loop_duration) + " seconds" +
          " | Frequency: " + std::to_string(1.0 / loop_duration) + " Hz");
    } else {
      spdlog::info(
          "Loop duration: " + std::to_string(loop_duration) + " seconds" +
          " | Frequency: " + std::to_string(1.0 / loop_duration) + " Hz");
    }
  }
}

void ControlUtils::display_images(
    const std::vector<trossen_ai_robot_devices::ImageData>& images) const {
  // Display all images together in a grid format using OpenCV
  if (!images.empty()) {
    // Determine grid size (rows x cols)
    int num_images = images.size();
    int grid_cols = static_cast<int>(std::ceil(std::sqrt(num_images)));
    int grid_rows =
        static_cast<int>(std::ceil(static_cast<float>(num_images) / grid_cols));

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
    cv::Mat grid_image(grid_height, grid_width, resized_images[0].type(),
                       cv::Scalar::all(0));

    // Copy each image into the grid
    for (int idx = 0; idx < num_images; ++idx) {
      int row = idx / grid_cols;
      int col = idx % grid_cols;
      cv::Rect roi(col * target_size.width, row * target_size.height,
                   target_size.width, target_size.height);
      cv::Mat rgb_image;
      cv::cvtColor(resized_images[idx], rgb_image, cv::COLOR_BGR2RGB);
      rgb_image.copyTo(grid_image(roi));
    }

    cv::imshow("Camera Grid", grid_image);
    cv::waitKey(1);
  }
}

}  // namespace trossen_sdk
