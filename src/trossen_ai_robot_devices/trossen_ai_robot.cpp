#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_ai_robot_devices/trossen_ai_driver.hpp"
#include <iostream>

namespace trossen_ai_robot_devices {


    namespace robot {

        TrossenAIWidowXRobot::TrossenAIWidowXRobot(const trossen_sdk_config::WidowXRobotConfig& config)
            : name_(config.name), ip_address_(config.ip_address) {
            
            robot_driver_ = std::make_unique<trossen_ai_robot_devices::TrossenAIArm>(config.name, config.ip_address, "follower");
            for (const auto& cam_config : config.cameras) {
                cameras_.emplace_back(cam_config.name, cam_config.serial, cam_config.width, cam_config.height, cam_config.fps, cam_config.use_depth);
            }
            std::cout << "TrossenAIWidowXRobot initialized with name: " << name_
                      << ", IP address: " << ip_address_ << std::endl;
        }

        void TrossenAIWidowXRobot::connect() {
            if (is_connected_) {
                std::cout << "Already connected to robot: " << name_ << std::endl;
                return;
            }
            robot_driver_->connect();
            for (auto& camera : cameras_) {
                camera.connect();
            }
            is_connected_ = true;
        }

        void TrossenAIWidowXRobot::disconnect() {
            std::cout << "Disconnecting from robot: " << name_ << std::endl;
            robot_driver_->disconnect();
            for (auto& camera : cameras_) {
                camera.disconnect();
            }
            is_connected_ = false;
        }

        void TrossenAIWidowXRobot::calibrate() {
        }

        void TrossenAIWidowXRobot::configure() {
            robot_driver_->stage_arm(); // Stage the arm to a safe position
        }

        trossen_ai_robot_devices::State TrossenAIWidowXRobot::get_observation() {
            trossen_ai_robot_devices::State state;
            std::vector<double> positions = robot_driver_->read("positions");
            state.observation_state = positions;
            state.action = robot_driver_->read("positions"); // Assuming action is read from the robot driver
            // Add camera logic here
            for (auto& camera : cameras_) {
                state.images.push_back(camera.async_read());
            }
            return state;
        }

        void TrossenAIWidowXRobot::send_action(const std::vector<double>& action) {
            robot_driver_->write("positions", action);
        }

        std::vector<std::string> TrossenAIWidowXRobot::get_joint_features() const{
            return robot_driver_->get_joint_names();
        }

        
        std::vector<std::string> TrossenAIWidowXRobot::get_observation_features() const{
            std::vector<std::string> features = get_joint_features();
            return features;
        }

        std::vector<std::string> TrossenAIWidowXRobot::get_camera_names() const {
            std::vector<std::string> camera_names;
            for (const auto& camera : cameras_) {
                camera_names.push_back(camera.name());
            }
            return camera_names;
        }

        TrossenAIBimanualWidowXRobot::TrossenAIBimanualWidowXRobot(const trossen_sdk_config::BimanualWidowXRobotConfig& config)
            : name_(config.name), right_ip_address_(config.right_ip_address), left_ip_address_(config.left_ip_address) {
            right_robot_driver_ = std::make_unique<trossen_ai_robot_devices::TrossenAIArm>(config.name, config.right_ip_address, "follower");
            left_robot_driver_ = std::make_unique<trossen_ai_robot_devices::TrossenAIArm>(config.name, config.left_ip_address, "follower");
            for (const auto& cam_config : config.cameras) {
                cameras_.emplace_back(cam_config.name, cam_config.serial, cam_config.width, cam_config.height, cam_config.fps, cam_config.use_depth);
            }
            spdlog::info("TrossenAIBimanualWidowXRobot initialized with name: {}, Right IP address: {}, Left IP address: {}", name_, right_ip_address_, left_ip_address_);
        }

        void TrossenAIBimanualWidowXRobot::connect() {
            if (is_connected_) {
                spdlog::info("Already connected to bimanual robot: {}", name_);
                return;
            }
            right_robot_driver_->connect();
            left_robot_driver_->connect();
            for (auto& camera : cameras_) {
                camera.connect();
            }
            is_connected_ = true;
        }

        void TrossenAIBimanualWidowXRobot::calibrate() {
        }

        void TrossenAIBimanualWidowXRobot::configure() {
            right_robot_driver_->stage_arm(); // Stage the right arm to a safe position
            left_robot_driver_->stage_arm(); // Stage the left arm to a safe position
        }

        trossen_ai_robot_devices::State TrossenAIBimanualWidowXRobot::get_observation() {
            trossen_ai_robot_devices::State state;
            std::vector<double> right_positions = right_robot_driver_->read("positions");
            std::vector<double> left_positions = left_robot_driver_->read("positions");
            state.observation_state.insert(state.observation_state.end(), right_positions.begin(), right_positions.end());
            state.observation_state.insert(state.observation_state.end(), left_positions.begin(), left_positions.end());
            state.action = right_positions; // Assuming action is read from the right robot driver
            // Add camera logic here
            for (auto& camera : cameras_) {
                state.images.push_back(camera.async_read());
            }
            return state;
        }

        void TrossenAIBimanualWidowXRobot::send_action(const std::vector<double>& action) {
            // Assuming action is split between the two arms
            if (action.size() < 14) {
                spdlog::error("Error: Expected at least 14 joint positions, got {}", action.size());
                return;
            }
            std::vector<double> right_action(action.begin(), action.begin() + 7);
            std::vector<double> left_action(action.begin() + 7, action.end());
            right_robot_driver_->write("positions", right_action);
            left_robot_driver_->write("positions", left_action);
        }

        void TrossenAIBimanualWidowXRobot::disconnect() {
            right_robot_driver_->stage_arm(); // Stop the right arm
            left_robot_driver_->stage_arm(); // Stop the left arm
            right_robot_driver_->disconnect();
            left_robot_driver_->disconnect();
            for (auto& camera : cameras_) {
                camera.disconnect();
            }
            is_connected_ = false;
            spdlog::info("Disconnected from bimanual robot: {}", name_);
        }

        std::vector<std::string> TrossenAIBimanualWidowXRobot::get_joint_features() const{
            std::vector<std::string> right_features_raw = right_robot_driver_->get_joint_names();
            std::vector<std::string> left_features_raw = left_robot_driver_->get_joint_names();
            std::vector<std::string> right_features, left_features;
            for (const auto& name : right_features_raw) {
                right_features.push_back("right_" + name);
            }
            for (const auto& name : left_features_raw) {
                left_features.push_back("left_" + name);
            }
            right_features.insert(right_features.end(), left_features.begin(), left_features.end());
            return right_features;
        }

        std::vector<std::string> TrossenAIBimanualWidowXRobot::get_observation_features() const {
            std::vector<std::string> right_features_raw = right_robot_driver_->get_joint_names();
            std::vector<std::string> left_features_raw = left_robot_driver_->get_joint_names();
            std::vector<std::string> right_features, left_features;
            for (const auto& name : right_features_raw) {
                right_features.push_back("right_" + name);
            }
            for (const auto& name : left_features_raw) {
                left_features.push_back("left_" + name);
            }
            right_features.insert(right_features.end(), left_features.begin(), left_features.end());
            return right_features;
        }

        std::vector<std::string> TrossenAIBimanualWidowXRobot::get_camera_names() const {
            std::vector<std::string> camera_names;
            for (const auto& camera : cameras_) {
                camera_names.push_back(camera.name());
            }
            return camera_names;
        }

    }

    namespace teleoperator {
        
        TrossenAIWidowXLeader::TrossenAIWidowXLeader(const trossen_sdk_config::WidowXLeaderConfig& config)
            : name_(config.name), ip_address_(config.ip_address) {
            robot_driver_ = std::make_unique<trossen_ai_robot_devices::TrossenAIArm>(config.name, config.ip_address, "leader");
        }

        void TrossenAIWidowXLeader::connect() {
            if (is_connected_) {
                spdlog::warn("Already connected to leader: {}", name_);
                return;
            }
            robot_driver_->connect();
        }
    

        void TrossenAIWidowXLeader::calibrate() {
        }

        void TrossenAIWidowXLeader::configure() {
            robot_driver_->stage_arm(); // Stage the arm to a safe position
            robot_driver_->write("external_efforts", std::vector<double>(7, 0.0));
        }

        std::vector<double> TrossenAIWidowXLeader::get_action() const {
            std::vector<double> positions = robot_driver_->read("positions");
            return positions;
        }

        void TrossenAIWidowXLeader::send_feedback() {

        }

        void TrossenAIWidowXLeader::disconnect() {
            robot_driver_->stage_arm(); // Stop the arm
            robot_driver_->disconnect();
            is_connected_ = false;
            spdlog::info("Disconnected from leader: {}", name_);
        }


        TrossenAIBimanualWidowXLeader::TrossenAIBimanualWidowXLeader(const trossen_sdk_config::BimanualWidowXLeaderConfig& config)
            : name_(config.name), left_ip_address_(config.left_ip_address), right_ip_address_(config.right_ip_address) {
            right_robot_driver_ = std::make_unique<trossen_ai_robot_devices::TrossenAIArm>(config.name, right_ip_address_, "leader");
            left_robot_driver_ = std::make_unique<trossen_ai_robot_devices::TrossenAIArm>(config.name, left_ip_address_, "leader");
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
        }
        void TrossenAIBimanualWidowXLeader::configure() {
            right_robot_driver_->stage_arm(); // Stage the right arm to a safe position
            left_robot_driver_->stage_arm(); // Stage the left arm to a safe position
            right_robot_driver_->write("external_efforts", std::vector<double>(7, 0.0));
            left_robot_driver_->write("external_efforts", std::vector<double>(7, 0.0));
        }
        std::vector<double> TrossenAIBimanualWidowXLeader::get_action() const {
            std::vector<double> right_positions = right_robot_driver_->read("positions");
            std::vector<double> left_positions = left_robot_driver_->read("positions");
            std::vector<double> action;
            action.insert(action.end(), right_positions.begin(), right_positions.end());
            action.insert(action.end(), left_positions.begin(), left_positions.end());
            return action;
        }
        void TrossenAIBimanualWidowXLeader::send_feedback() {
            // Implement feedback logic if needed
        }
        void TrossenAIBimanualWidowXLeader::disconnect() {
            right_robot_driver_->stage_arm(); // Stop the right arm
            left_robot_driver_->stage_arm(); // Stop the left arm
            right_robot_driver_->disconnect();
            left_robot_driver_->disconnect();
            is_connected_ = false;
            spdlog::info("Disconnected from bimanual leader: {}", name_);
        }

    }  // namespace teleoperator
}  // namespace trossen_ai_robot_devices





