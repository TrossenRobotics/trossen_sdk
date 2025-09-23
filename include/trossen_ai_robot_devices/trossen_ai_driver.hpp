// Copyright 2025 Trossen Robotics
#ifndef INCLUDE_TROSSEN_AI_ROBOT_DEVICES_TROSSEN_AI_DRIVER_HPP_
#define INCLUDE_TROSSEN_AI_ROBOT_DEVICES_TROSSEN_AI_DRIVER_HPP_
#include <spdlog/spdlog.h>

#include <iostream>
#include <string>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"
#include "trossen_sdk_utils/constants.hpp"

namespace trossen_ai_robot_devices {
/**
 * @brief Class representing a Trossen AI Robotic Arm
 * This is used as a wrapper around the TrossenArmDriver from the libtrossen_arm
 * library to provide a simplified interface for connecting, disconnecting,
 * reading, writing, and staging the robotic arm.
 */
class TrossenAIArm {
 public:
  /**
   * @brief Constructor for TrossenAIArm
   * @param name Name of the robotic arm
   * @param ip_address IP address of the robotic arm
   * @param model Model of the robotic arm (e.g., "leader", "follower")
   * Initializes the TrossenAIArm with the given parameters
   */
  TrossenAIArm(const std::string& name, const std::string& ip_address,
               const std::string& model);

  /**
   * @brief Move constructor and move assignment operator
   *
   * Enables safe and efficient transfer of ownership for resources like
   * ROS nodes, unique_ptr drivers, or file handles. Moving is allowed,
   * as it ensures only one object instance manages the underlying resources.
   *
   * noexcept ensures STL containers (like std::vector) can optimize moves.
   */
  TrossenAIArm(TrossenAIArm&&) noexcept = default;

  /** Move assignment operator */
  TrossenAIArm& operator=(TrossenAIArm&&) noexcept = default;

  /**
   * @brief Copy constructor and copy assignment operator (deleted)
   *
   * Copying is explicitly disabled to prevent issues like:
   * - Double deletion of unique resources (e.g., std::unique_ptr)
   * - Multiple ROS nodes accessing the same hardware
   * - Undefined behavior from duplicating file handles or hardware states
   *
   * This enforces unique ownership and avoids resource mismanagement.
   */
  TrossenAIArm(const TrossenAIArm&) = delete;
  TrossenAIArm& operator=(const TrossenAIArm&) = delete;

  /**
   * @brief Connect to the robotic arm
   * Configures and initializes the connection to the arm
   * Sets the initial position of the arm
   */
  void connect();
  /**
   * @brief Disconnect from the robotic arm
   * Moves the arm to a sleep position and puts it in idle mode
   */
  void disconnect();

  /**
   * @brief Read data from the robotic arm
   * @param data_name Name of the data to read (e.g., trossen_sdk::POSITION,
   * "trossen_sdk::VELOCITY", trossen_sdk::EXTERNAL_EFFORT)
   * @return Vector of doubles containing the requested data
   * Reads the specified data from the arm and returns it as a vector of doubles
   */
  std::vector<double> read(std::string data_name);

  /**
   * @brief Write data to the robotic arm
   * @param data_name Name of the data to write (e.g., trossen_sdk::POSITION,
   * "trossen_sdk::VELOCITY", trossen_sdk::EXTERNAL_EFFORT)
   * @param data Vector of doubles containing the data to write
   * Writes the specified data to the arm
   */
  void write(const std::string& data_name, const std::vector<double>& data);

  /**
   * @brief Stage the arm to a default safe position
   * Moves the arm to a predefined position for safety
   */
  void stage_arm();

  /**
   * @brief Get the name of the robotic arm
   * @return Name of the arm as a string
   */
  std::string get_name() const { return name_; }

  /**
   * @brief Get the joint names of the robotic arm
   * @return Vector of strings containing the joint names
   */
  std::vector<std::string> get_joint_names() const;

  /// @brief Get the number of joints in the robotic arm
  /// @return Number of joints as an integer
  int get_num_joints() const { return num_joints_; }

 private:
  /// @brief Name of the TrossenAIArm
  std::string name_;

  /// @brief IP address of the robotic arm
  std::string ip_address_;

  /// @brief Model of the robotic arm (e.g., "leader", "follower")
  std::string model_;

  /// @brief The underlying TrossenArmDriver instance
  trossen_arm::TrossenArmDriver driver_;

  /// @brief Track connection status
  bool is_connected_ = false;

  // TODO(shantanuparab-tr) [TDS-32] Make time_to_move_ configurable
  /// @brief Time to move for position/velocity/effort commands
  float time_to_move_ = 0.1;

  /// @brief Number of joints in the robotic arm
  int num_joints_;
};

}  // namespace trossen_ai_robot_devices

#endif  // INCLUDE_TROSSEN_AI_ROBOT_DEVICES_TROSSEN_AI_DRIVER_HPP_
