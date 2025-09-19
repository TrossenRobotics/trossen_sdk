// Copyright 2025 Trossen Robotics
#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"

#include <iostream>

#include "trossen_ai_robot_devices/trossen_ai_driver.hpp"

namespace trossen_ai_robot_devices {

namespace robot {

std::vector<trossen_ai_robot_devices::CameraType> TrossenRobot::get_camera_features() const {
  std::vector<trossen_ai_robot_devices::CameraType> camera_names;
  for (const auto& camera : cameras_) {
    camera_names.push_back(
        {camera->name(), camera->is_using_depth() ? "depth" : "color"});
  }
  return camera_names;
}

TrossenAIWidowXRobot::TrossenAIWidowXRobot(
    const trossen_sdk_config::WidowXRobotConfig& config)
    : name_(config.name), ip_address_(config.ip_address) {
  robot_driver_ = std::make_unique<trossen_ai_robot_devices::TrossenAIArm>(
      config.name, config.ip_address, trossen_sdk::FOLLOWER_MODEL);
  // Check the camera interfaces and create camera objects
  if (config.camera_interface == "realsense") {
    for (const auto& cam_config : config.cameras) {
      cameras_.emplace_back(
          std::make_unique<trossen_ai_robot_devices::RealsenseCamera>(
              cam_config.name, cam_config.serial, cam_config.width,
              cam_config.height, cam_config.fps, cam_config.use_depth));
    }
  } else if (config.camera_interface == "opencv") {
    for (const auto& cam_config : config.cameras) {
      cameras_.emplace_back(
          std::make_unique<trossen_ai_robot_devices::OpenCVCamera>(
              cam_config.name, cam_config.serial, cam_config.width,
              cam_config.height, cam_config.fps, cam_config.use_depth));
    }
  } else {
    spdlog::error(
        "Unknown camera interface: {}. Supported interfaces are 'realsense' "
        "and 'opencv'.",
        config.camera_interface);
    throw std::runtime_error("Unknown camera interface: " +
                             config.camera_interface);
  }
  spdlog::info("TrossenAIWidowXRobot initialized with name: {}, IP address: {}",
               name_, ip_address_);
}

void TrossenAIWidowXRobot::connect() {
  // Connect to the robot arm and cameras
  if (is_connected_) {
    spdlog::info("Already connected to robot: {}", name_);
    return;
  }
  robot_driver_->connect();
  for (auto& camera : cameras_) {
    camera->connect();
  }
  is_connected_ = true;
}

void TrossenAIWidowXRobot::disconnect() {
  // Disconnect from the robot arm and cameras
  spdlog::info("Disconnecting from robot: {}", name_);
  robot_driver_->disconnect();
  for (auto& camera : cameras_) {
    camera->disconnect();
  }
  is_connected_ = false;
}

void TrossenAIWidowXRobot::calibrate() {
  // TODO(shantanuparab-tr) Implement calibration logic if needed
}

void TrossenAIWidowXRobot::configure() {
  // Stage the arm to a safe position
  robot_driver_->stage_arm();
}

void TrossenAIWidowXRobot::get_observation(
    trossen_ai_robot_devices::State* state) {
  // Read joint positions from the robot arm
  state->observation_state = robot_driver_->read(trossen_sdk::POSITION);
  // Read camera images
  for (auto& camera : cameras_) {
    state->images.push_back(camera->async_read());
  }
}

void TrossenAIWidowXRobot::send_action(const std::vector<double>& action) {
  robot_driver_->write(trossen_sdk::POSITION, action);
}

// TODO(shantanuparab-tr) Improve this logic or delete the function if not
// needed
std::vector<std::string> TrossenAIWidowXRobot::get_joint_features() const {
  return robot_driver_->get_joint_names();
}

// TODO(shantanuparab-tr) Improve this logic or delete the function if not
// needed
std::vector<std::string> TrossenAIWidowXRobot::get_observation_features()
    const {
  std::vector<std::string> features = get_joint_features();
  return features;
}

TrossenAIBimanualWidowXRobot::TrossenAIBimanualWidowXRobot(
    const trossen_sdk_config::BimanualWidowXRobotConfig& config)
    : name_(config.name),
      right_ip_address_(config.right_ip_address),
      left_ip_address_(config.left_ip_address) {
  right_robot_driver_ =
      std::make_unique<trossen_ai_robot_devices::TrossenAIArm>(
          config.name, config.right_ip_address, trossen_sdk::FOLLOWER_MODEL);
  left_robot_driver_ = std::make_unique<trossen_ai_robot_devices::TrossenAIArm>(
      config.name, config.left_ip_address, trossen_sdk::FOLLOWER_MODEL);
  // Check the camera interfaces and create camera objects
  if (config.camera_interface == "realsense") {
    for (const auto& cam_config : config.cameras) {
      cameras_.emplace_back(
          std::make_unique<trossen_ai_robot_devices::RealsenseCamera>(
              cam_config.name, cam_config.serial, cam_config.width,
              cam_config.height, cam_config.fps, cam_config.use_depth));
    }
  } else if (config.camera_interface == "opencv") {
    for (const auto& cam_config : config.cameras) {
      cameras_.emplace_back(
          std::make_unique<trossen_ai_robot_devices::OpenCVCamera>(
              cam_config.name, cam_config.serial, cam_config.width,
              cam_config.height, cam_config.fps, cam_config.use_depth));
    }
  } else {
    spdlog::error(
        "Unknown camera interface: {}. Supported interfaces are 'realsense' "
        "and 'opencv'.",
        config.camera_interface);
    throw std::runtime_error("Unknown camera interface: " +
                             config.camera_interface);
  }
  spdlog::info(
      "TrossenAIBimanualWidowXRobot initialized with name: {}, Right IP "
      "address: {}, Left IP "
      "address: {}",
      name_, right_ip_address_, left_ip_address_);
}

void TrossenAIBimanualWidowXRobot::connect() {
  if (is_connected_) {
    spdlog::info("Already connected to bimanual robot: {}", name_);
    return;
  }
  right_robot_driver_->connect();
  left_robot_driver_->connect();
  for (auto& camera : cameras_) {
    camera->connect();
  }
  is_connected_ = true;
}

void TrossenAIBimanualWidowXRobot::calibrate() {
  // TODO(shantanuparab-tr) Implement calibration logic if needed
  // Compliant Gripper Calibration
}

void TrossenAIBimanualWidowXRobot::configure() {
  // Stage both arms to safe positions
  right_robot_driver_->stage_arm();
  left_robot_driver_->stage_arm();
}

void TrossenAIBimanualWidowXRobot::get_observation(
    trossen_ai_robot_devices::State* state) {
  // Read joint positions from both robot arms and combine them
  std::vector<double> right_positions =
      right_robot_driver_->read(trossen_sdk::POSITION);
  std::vector<double> left_positions =
      left_robot_driver_->read(trossen_sdk::POSITION);
  state->observation_state.insert(state->observation_state.end(),
                                  right_positions.begin(),
                                  right_positions.end());
  state->observation_state.insert(state->observation_state.end(),
                                  left_positions.begin(), left_positions.end());
  // Read camera images
  for (auto& camera : cameras_) {
    state->images.push_back(camera->async_read());
  }
}

void TrossenAIBimanualWidowXRobot::send_action(
    const std::vector<double>& action) {
  // Split the action vector and send commands to both robot arms
  if (action.size() < right_robot_driver_->get_num_joints() +
                          left_robot_driver_->get_num_joints()) {
    spdlog::error("Error: Expected at least {} joint positions, got {}",
                  right_robot_driver_->get_num_joints() +
                      left_robot_driver_->get_num_joints(),
                  action.size());
    return;
  }
  std::vector<double> right_action(
      action.begin(), action.begin() + right_robot_driver_->get_num_joints());
  std::vector<double> left_action(
      action.begin() + left_robot_driver_->get_num_joints(), action.end());
  right_robot_driver_->write(trossen_sdk::POSITION, right_action);
  left_robot_driver_->write(trossen_sdk::POSITION, left_action);
}

void TrossenAIBimanualWidowXRobot::disconnect() {
  // Stage both arms to safe positions and disconnect
  right_robot_driver_->stage_arm();
  left_robot_driver_->stage_arm();
  right_robot_driver_->disconnect();
  left_robot_driver_->disconnect();
  for (auto& camera : cameras_) {
    camera->disconnect();
  }
  is_connected_ = false;
  spdlog::info("Disconnected from bimanual robot: {}", name_);
}

std::vector<std::string> TrossenAIBimanualWidowXRobot::get_joint_features()
    const {
  std::vector<std::string> right_features_raw =
      right_robot_driver_->get_joint_names();
  std::vector<std::string> left_features_raw =
      left_robot_driver_->get_joint_names();
  std::vector<std::string> features;
  // Prefix joint names with "right_" and "left_" and suffix with ".pos"
  // This is to allow compatibility with LeRobot replay and visualization tools
  for (const auto& name : right_features_raw) {
    features.push_back("right_" + name + ".pos");
  }
  for (const auto& name : left_features_raw) {
    features.push_back("left_" + name + ".pos");
  }
  return features;
}

// TODO(shantanuparab-tr) Improve this logic or delete the function if not
// needed
std::vector<std::string>
TrossenAIBimanualWidowXRobot::get_observation_features() const {
  std::vector<std::string> right_features_raw =
      right_robot_driver_->get_joint_names();
  std::vector<std::string> left_features_raw =
      left_robot_driver_->get_joint_names();
  std::vector<std::string> right_features, left_features;
  for (const auto& name : right_features_raw) {
    right_features.push_back("right_" + name + ".pos");
  }
  for (const auto& name : left_features_raw) {
    left_features.push_back("left_" + name + ".pos");
  }
  right_features.insert(right_features.end(), left_features.begin(),
                        left_features.end());
  return right_features;
}

}  // namespace robot

namespace teleoperator {

TrossenAIWidowXLeader::TrossenAIWidowXLeader(
    const trossen_sdk_config::WidowXLeaderConfig& config)
    : name_(config.name), ip_address_(config.ip_address) {
  robot_driver_ = std::make_unique<trossen_ai_robot_devices::TrossenAIArm>(
      config.name, config.ip_address, trossen_sdk::LEADER_MODEL);

  spdlog::info(
      "TrossenAIWidowXLeader initialized with name: {}, IP address: {}", name_,
      ip_address_);
}

void TrossenAIWidowXLeader::connect() {
  if (is_connected_) {
    spdlog::warn("Already connected to leader: {}", name_);
    return;
  }
  robot_driver_->connect();
}

void TrossenAIWidowXLeader::calibrate() {}

void TrossenAIWidowXLeader::configure() {
  robot_driver_->stage_arm();
  robot_driver_->write(
      trossen_sdk::EXTERNAL_EFFORT,
      std::vector<double>(robot_driver_->get_num_joints(), 0.0));
}

std::vector<double> TrossenAIWidowXLeader::get_action() const {
  std::vector<double> positions = robot_driver_->read(trossen_sdk::POSITION);
  return positions;
}

void TrossenAIWidowXLeader::send_feedback() {}

void TrossenAIWidowXLeader::disconnect() {
  robot_driver_->stage_arm();
  robot_driver_->disconnect();
  is_connected_ = false;
  spdlog::info("Disconnected from leader: {}", name_);
}

TrossenAIBimanualWidowXLeader::TrossenAIBimanualWidowXLeader(
    const trossen_sdk_config::BimanualWidowXLeaderConfig& config)
    : name_(config.name),
      left_ip_address_(config.left_ip_address),
      right_ip_address_(config.right_ip_address) {
  right_robot_driver_ =
      std::make_unique<trossen_ai_robot_devices::TrossenAIArm>(
          config.name, right_ip_address_, trossen_sdk::LEADER_MODEL);
  left_robot_driver_ = std::make_unique<trossen_ai_robot_devices::TrossenAIArm>(
      config.name, left_ip_address_, trossen_sdk::LEADER_MODEL);

  spdlog::info(
      "TrossenAIBimanualWidowXLeader initialized with name: {}, Right IP "
      "address: {}, Left IP "
      "address: {}",
      name_, right_ip_address_, left_ip_address_);
}
void TrossenAIBimanualWidowXLeader::connect() {
  if (is_connected_) {
    spdlog::warn("Already connected to bimanual leader: {}", name_);
    return;
  }
  right_robot_driver_->connect();
  left_robot_driver_->connect();
}
void TrossenAIBimanualWidowXLeader::calibrate() {
  // TODO(shantanuparab-tr) Implement calibration logic if needed
}
void TrossenAIBimanualWidowXLeader::configure() {
  right_robot_driver_->stage_arm();  // Stage the right arm to a safe position
  left_robot_driver_->stage_arm();   // Stage the left arm to a safe position
  right_robot_driver_->write(
      trossen_sdk::EXTERNAL_EFFORT,
      std::vector<double>(right_robot_driver_->get_num_joints(), 0.0));
  left_robot_driver_->write(
      trossen_sdk::EXTERNAL_EFFORT,
      std::vector<double>(left_robot_driver_->get_num_joints(), 0.0));
}
std::vector<double> TrossenAIBimanualWidowXLeader::get_action() const {
  std::vector<double> right_positions =
      right_robot_driver_->read(trossen_sdk::POSITION);
  std::vector<double> left_positions =
      left_robot_driver_->read(trossen_sdk::POSITION);
  std::vector<double> action;
  action.insert(action.end(), right_positions.begin(), right_positions.end());
  action.insert(action.end(), left_positions.begin(), left_positions.end());
  return action;
}
void TrossenAIBimanualWidowXLeader::send_feedback() {
  // Implement feedback logic if needed
}
void TrossenAIBimanualWidowXLeader::disconnect() {
  right_robot_driver_->stage_arm();  // Stop the right arm
  left_robot_driver_->stage_arm();   // Stop the left arm
  right_robot_driver_->disconnect();
  left_robot_driver_->disconnect();
  is_connected_ = false;
  spdlog::info("Disconnected from bimanual leader: {}", name_);
}

}  // namespace teleoperator
}  // namespace trossen_ai_robot_devices
