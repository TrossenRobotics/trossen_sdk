#ifndef TROSSEN_AI_ROBOT_HPP
#define TROSSEN_AI_ROBOT_HPP
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/io/file.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <parquet/arrow/reader.h>  // Needed for OpenFile and FileReader
#include <parquet/arrow/writer.h>
#include <spdlog/spdlog.h>

#include <any>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"
#include "trossen_ai_robot_devices/trossen_ai_cameras.hpp"
#include "trossen_ai_robot_devices/trossen_ai_driver.hpp"
#include "trossen_sdk_utils/config_utils.hpp"
#include "trossen_sdk_utils/constants.hpp"

namespace trossen_ai_robot_devices {

/// @brief Data structure to hold the robot state including joint positions,
/// action, and images for each frame
struct State {
  /// @brief Observation state of the robot (e.g., joint positions)
  std::vector<double> observation_state;
  /// @brief Action to be taken by the robot
  std::vector<double> action;
  /// @brief Images captured for the frame
  std::vector<ImageData> images;
};

struct CameraType {
  /// @brief Name of the camera
  std::string name;
  /// @brief Type of the camera (e.g., "color", "depth")
  std::string type;
};

namespace teleoperator {

class TrossenLeader {
 public:
  /** @brief Connect to the robot leader */
  virtual void connect() = 0;
  /** @brief Disconnect from the robot leader */
  virtual void disconnect() = 0;
  /** @brief Calibrate the robot leader */
  virtual void calibrate() = 0;
  /** @brief Configure the robot leader */
  virtual void configure() = 0;
  /** @brief Get the current action from the robot leader */
  virtual std::vector<double> get_action() const = 0;

  // TODO [TDS-33] Implement Force Feedback
  /** @brief Send feedback to the robot leader */
  virtual void send_feedback() = 0;
  /** @brief Get the name of the robot leader */
  virtual std::string name() const = 0;
};

class TrossenAIWidowXLeader : public TrossenLeader {
 public:
  /**
   * @brief Constructor for TrossenAIWidowXLeader
   * @param config Configuration parameters for the robot leader
   */
  TrossenAIWidowXLeader(const trossen_sdk_config::WidowXLeaderConfig& config);

  /** @brief Connect to the robot leader */
  void connect() override;
  /** @brief Calibrate the robot leader */
  void calibrate() override;
  /** @brief Configure the robot leader */
  void configure() override;
  /** @brief Get the current action from the robot leader */
  std::vector<double> get_action() const override;
  /** @brief Send feedback to the robot leader */
  void send_feedback() override;
  /** @brief Disconnect from the robot leader */
  void disconnect() override;
  /** @brief Get the name of the robot leader */
  std::string name() const override { return name_; }

 private:
  /// @brief Name of the robot leader
  std::string name_;
  /// @brief IP address of the robot leader
  std::string ip_address_;
  /// @brief Driver to control the robot arm
  std::unique_ptr<trossen_ai_robot_devices::TrossenAIArm> robot_driver_;

  /// @brief Track connection status
  bool is_connected_ = false;
};

class TrossenAIBimanualWidowXLeader : public TrossenLeader {
 public:
  /**
   * @brief Constructor for TrossenAIBimanualWidowXLeader
   * @param config Configuration parameters for the bimanual robot leader
   */
  TrossenAIBimanualWidowXLeader(
      const trossen_sdk_config::BimanualWidowXLeaderConfig& config);
  /** @brief Connect to the bimanual robot leader */
  void connect() override;
  /** @brief Calibrate the bimanual robot leader */
  void calibrate() override;
  /** @brief Configure the bimanual robot leader */
  void configure() override;
  /** @brief Get the current action from the bimanual robot leader */
  std::vector<double> get_action() const override;
  /** @brief Send feedback to the bimanual robot leader */
  void send_feedback() override;
  /** @brief Disconnect from the bimanual robot leader */
  void disconnect() override;
  /** @brief Get the name of the bimanual robot leader */
  std::string name() const override { return name_; }

 private:
  /// @brief Name of the bimanual robot leader
  std::string name_;
  /// @brief IP address of the left robot arm
  std::string left_ip_address_;
  /// @brief IP address of the right robot arm
  std::string right_ip_address_;
  /// @brief Driver to control the right robot arm
  std::unique_ptr<trossen_ai_robot_devices::TrossenAIArm> right_robot_driver_;
  /// @brief Driver to control the left robot arm
  std::unique_ptr<trossen_ai_robot_devices::TrossenAIArm> left_robot_driver_;
  /// @brief Track connection status
  bool is_connected_ = false;
};

}  // namespace teleoperator

namespace robot {
/**
 * @brief Abstract base class representing a generic robot
 * This class defines the interface for connecting, disconnecting, calibrating,
 * configuring, and interacting with a robot. It also provides methods to
 * retrieve the robot's name, observation state, joint features, observation
 * features, and camera names.
 */
class TrossenRobot {
 public:
  /** @brief Connect to the robot */
  virtual void connect() = 0;

  /** @brief Disconnect from the robot */
  virtual void disconnect() = 0;

  /** @brief Calibrate the robot */
  virtual void calibrate() = 0;

  /** @brief Configure the robot */
  virtual void configure() = 0;

  /** @brief Get the name of the robot
   * @return Name of the robot as a string
   */
  virtual std::string name() const = 0;
  /** @brief Get the current observation state of the robot
   * @return State structure containing the current observation state
   */
  virtual void get_observation(trossen_ai_robot_devices::State& state) = 0;

  /** @brief Send an action command to the robot
   * @param action Vector of doubles representing the action to be sent
   */
  virtual void send_action(const std::vector<double>& action) = 0;

  /** @brief Get the joint feature names of the robot
   * @return Vector of joint feature names
   */
  virtual std::vector<std::string> get_joint_features() const = 0;

  /** @brief Get the observation feature names of the robot
   * @return Vector of observation feature names
   */
  virtual std::vector<std::string> get_observation_features() const = 0;

  /** @brief Get the camera names of the robot
   * @return Vector of pairs containing camera name and type (e.g., "depth" or
   * "color")
   */
  virtual std::vector<trossen_ai_robot_devices::CameraType>
  get_camera_features() const = 0;
};

class TrossenAIWidowXRobot : public TrossenRobot {
 public:
  /**
   * @brief Constructor for TrossenAIWidowXRobot
   * @param config Configuration parameters for the robot
   */
  TrossenAIWidowXRobot(const trossen_sdk_config::WidowXRobotConfig& config);

  /** @brief Connect to the robot */
  void connect() override;

  /** @brief Disconnect from the robot */
  void disconnect() override;

  /** @brief Calibrate the robot */
  void calibrate() override;

  /** @brief Configure the robot */
  void configure() override;

  /** @brief Get the name of the robot
   * @return Name of the robot as a string
   */
  std::string name() const override { return name_; }

  /** @brief Get the current observation state of the robot
   * @return State structure containing the current observation state
   */
  void get_observation(trossen_ai_robot_devices::State& state) override;

  /** @brief Send an action command to the robot
   * @param action Vector of doubles representing the action to be sent
   */
  void send_action(const std::vector<double>& action) override;

  /** @brief Get the joint feature names of the robot
   * @return Vector of joint feature names
   */
  std::vector<std::string> get_joint_features() const override;

  /** @brief Get the observation feature names of the robot
   * @return Vector of observation feature names
   */
  std::vector<std::string> get_observation_features() const override;

  /** @brief Get the camera names of the robot
   * @return Vector of pairs containing camera name and type (e.g., "depth" or
   * "color")
   */
  std::vector<trossen_ai_robot_devices::CameraType> get_camera_features()
      const override;

 private:
  /// @brief Name of the robot
  std::string name_;
  /// @brief IP address of the robot
  std::string ip_address_;
  /// @brief Driver to control the robot arm
  std::unique_ptr<trossen_ai_robot_devices::TrossenAIArm> robot_driver_;
  /// @brief Track connection status
  bool is_connected_ = false;
  /// @brief Cameras attached to the robot
  std::vector<std::unique_ptr<trossen_ai_robot_devices::TrossenAICamera>>
      cameras_;
};

class TrossenAIBimanualWidowXRobot : public TrossenRobot {
 public:
  /**
   * @brief Constructor for TrossenAIBimanualWidowXRobot
   * @param config Configuration parameters for the bimanual robot
   */
  TrossenAIBimanualWidowXRobot(
      const trossen_sdk_config::BimanualWidowXRobotConfig& config);

  /** @brief Connect to the bimanual robot */
  void connect() override;

  /** @brief Disconnect from the bimanual robot */
  void disconnect() override;

  /** @brief Calibrate the bimanual robot */
  void calibrate() override;

  /** @brief Configure the bimanual robot */
  void configure() override;

  /** @brief Get the name of the bimanual robot
   * @return Name of the robot as a string
   */
  std::string name() const override { return name_; }

  /** @brief Get the current observation state of the bimanual robot
   * @return State structure containing the current observation state
   */
  void get_observation(trossen_ai_robot_devices::State& state) override;

  /** @brief Send an action command to the bimanual robot
   * @param action Vector of doubles representing the action to be sent
   */
  void send_action(const std::vector<double>& action) override;

  /** @brief Get the joint feature names of the bimanual robot
   * @return Vector of joint feature names
   */
  std::vector<std::string> get_joint_features() const override;

  /** @brief Get the observation feature names of the bimanual robot
   * @return Vector of observation feature names
   */
  std::vector<std::string> get_observation_features() const override;

  /** @brief Get the camera names of the bimanual robot
   * @return Vector of pairs containing camera name and type (e.g., "depth" or
   * "color")
   */
  std::vector<trossen_ai_robot_devices::CameraType> get_camera_features()
      const override;

 private:
  /// @brief Name of the bimanual robot
  std::string name_;

  /// @brief IP address of the left robot arm
  std::string left_ip_address_;

  /// @brief IP address of the right robot arm
  std::string right_ip_address_;

  /// @brief Driver to control the right robot arm
  std::unique_ptr<trossen_ai_robot_devices::TrossenAIArm> right_robot_driver_;

  /// @brief Driver to control the left robot arm
  std::unique_ptr<trossen_ai_robot_devices::TrossenAIArm> left_robot_driver_;

  /// @brief Track connection status
  bool is_connected_ = false;

  /// @brief Cameras attached to the bimanual robot
  std::vector<std::unique_ptr<trossen_ai_robot_devices::TrossenAICamera>>
      cameras_;
};
}  // namespace robot
}  // namespace trossen_ai_robot_devices

#endif  // TROSSEN_AI_ROBOT_HPP