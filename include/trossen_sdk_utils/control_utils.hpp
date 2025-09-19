// Copyright 2025 Trossen Robotics
#ifndef INCLUDE_TROSSEN_SDK_UTILS_CONTROL_UTILS_HPP_
#define INCLUDE_TROSSEN_SDK_UTILS_CONTROL_UTILS_HPP_

#include <spdlog/spdlog.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "trossen_ai_robot_devices/trossen_ai_cameras.hpp"
#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_dataset/dataset.hpp"

namespace trossen_sdk {

class ControlUtils {
 public:
  /**
   * @brief Control loop for teleoperated robot with dataset recording
   * @param robot Shared pointer to the robot object
   * @param teleop_robot Shared pointer to the teleoperation robot object
   * @param control_time Control loop time in seconds
   * @param dataset Reference to the dataset object for recording
   * @param display_cameras Flag to indicate if camera images should be
   * displayed
   * @param fps Frames per second for video conversion
   * This function runs a control loop for a teleoperated robot, capturing
   * images and recording data to the dataset. It also optionally displays
   * camera images.
   */
  void control_loop(
      std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot> robot,
      std::shared_ptr<trossen_ai_robot_devices::teleoperator::TrossenLeader>
          teleop_robot,
      float control_time, trossen_dataset::TrossenAIDataset& dataset,
      bool display_cameras, double fps);
  /**
   * @brief Control loop for teleoperated robot without dataset recording for
   * teleoperation only
   * @param robot Shared pointer to the robot object
   * @param teleop_robot Shared pointer to the teleoperation robot object
   * @param control_time Control loop time in seconds
   * @param display_cameras Flag to indicate if camera images should be
   * displayed
   * @param fps Frames per second for control loop
   * This function runs a control loop for a teleoperated robot, capturing
   * images. It also optionally displays camera images.
   */
  void control_loop(
      std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot> robot,
      std::shared_ptr<trossen_ai_robot_devices::teleoperator::TrossenLeader>
          teleop_robot,
      float control_time, bool display_cameras, double fps);
  /**
   * @brief Busy-wait until the next loop iteration based on the desired
   * frequency
   * @param loop_start Time point when the loop iteration started
   * @param frequency Desired frequency in Hz
   * This function calculates the elapsed time since the loop started and sleeps
   * for the remaining time to maintain a consistent loop frequency.
   */
  inline void busy_wait_until(
      const std::chrono::steady_clock::time_point& loop_start,
      double frequency) {
    auto desired_duration = std::chrono::duration<double>(1.0 / frequency);
    auto elapsed = std::chrono::steady_clock::now() - loop_start;
    auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
        desired_duration - elapsed);
    if (remaining.count() > 0) {
      std::this_thread::sleep_for(remaining);
    }
  }

  /**
   * @brief Display images from the robot's cameras
   * @param images Vector of ImageData objects containing images from the
   * cameras This function displays the images from the robot's cameras using
   * OpenCV.
   */
  void display_images(
      const std::vector<trossen_ai_robot_devices::ImageData>& images) const;
};

}  // namespace trossen_sdk

#endif  // INCLUDE_TROSSEN_SDK_UTILS_CONTROL_UTILS_HPP_
