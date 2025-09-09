#ifndef TROSSEN_AI_ROBOT_HPP
#define TROSSEN_AI_ROBOT_HPP
#include <cmath>
#include <iostream>
#include <vector>
#include <string>
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#include <parquet/arrow/reader.h>  // Needed for OpenFile and FileReader
#include <arrow/io/file.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <thread>
#include <chrono>
#include "libtrossen_arm/trossen_arm.hpp"
#include "trossen_ai_robot_devices/trossen_ai_driver.hpp"
#include "trossen_ai_robot_devices/trossen_ai_cameras.hpp"
#include "trossen_sdk_utils/config_utils.hpp"
#include <any>
#include <spdlog/spdlog.h>

namespace trossen_ai_robot_devices {


struct State{
    std::vector<double> observation_state;  // Joint positions
    std::vector<double> action;             // Action to be taken
    std::vector<ImageData> images; // Images captured during the episode
};

namespace teleoperator {

    class TrossenLeader{

        public:

            virtual void connect() = 0;
            virtual void disconnect() = 0;
            virtual void calibrate() = 0;
            virtual void configure() = 0;
            virtual std::vector<double> get_action() const = 0;
            virtual void send_feedback() = 0;
            virtual std::string name() const = 0;
    };

    class TrossenAIWidowXLeader : public TrossenLeader {

        public: 
            TrossenAIWidowXLeader(const trossen_sdk_config::WidowXLeaderConfig& config);
            void connect() override;
            void calibrate() override;
            void configure() override;
            std::vector<double> get_action() const override;
            void send_feedback() override;
            void disconnect() override;
            std::string name() const override { return name_; }

        private:
            std::string name_;
            std::string ip_address_;
            std::unique_ptr<trossen_ai_robot_devices::TrossenAIArm> robot_driver_;  // Assuming TrossenArmDriver is the class to control the arm

            bool is_connected_ = false;  // Track connection status
    };

    class TrossenAIBimanualWidowXLeader : public TrossenLeader {

        public: 
            TrossenAIBimanualWidowXLeader(const trossen_sdk_config::BimanualWidowXLeaderConfig& config);
            void connect() override;
            void calibrate() override;
            void configure() override;
            std::vector<double> get_action() const override;
            void send_feedback() override;
            void disconnect() override;
            std::string name() const override { return name_; }

        private:
            std::string name_;
            std::string left_ip_address_;
            std::string right_ip_address_;
            std::unique_ptr<trossen_ai_robot_devices::TrossenAIArm> right_robot_driver_;  // Assuming TrossenArmDriver is the class to control the arm
            std::unique_ptr<trossen_ai_robot_devices::TrossenAIArm> left_robot_driver_;  // Assuming TrossenArmDriver is the class to control the arm

            bool is_connected_ = false;  // Track connection status
    };

}

namespace robot {

    class TrossenRobot {

        public:
            virtual void connect() = 0;
            virtual void disconnect() = 0;
            virtual void calibrate() = 0;
            virtual void configure() = 0;
            virtual trossen_ai_robot_devices::State get_observation() = 0;
            virtual void send_action(const std::vector<double>& action) = 0;
            virtual std::string name() const = 0;
            virtual std::vector<std::string> get_joint_features() const = 0;
            virtual std::vector<std::string> get_observation_features() const = 0;
            virtual std::vector<std::pair<std::string, std::string>> get_camera_names() const = 0;
    };

    class TrossenAIWidowXRobot : public TrossenRobot {
    public:
        TrossenAIWidowXRobot(const trossen_sdk_config::WidowXRobotConfig& config);
        void connect() override;
        void disconnect() override;
        void calibrate() override;
        void configure() override;
        std::string name() const override { return name_; }
        trossen_ai_robot_devices::State get_observation() override;
        void send_action(const std::vector<double>& action) override;
        std::vector<std::string> get_joint_features() const override;
        std::vector<std::string> get_observation_features() const override;
        std::vector<std::pair<std::string, std::string>> get_camera_names() const override;

    private:
        std::string name_;
        std::string ip_address_;
        std::unique_ptr<trossen_ai_robot_devices::TrossenAIArm> robot_driver_;
        bool is_connected_ = false;  // Track connection status
        std::vector<trossen_ai_robot_devices::TrossenAICamera> cameras_;
    };

    class TrossenAIBimanualWidowXRobot : public TrossenRobot  {
    public:
        TrossenAIBimanualWidowXRobot(const trossen_sdk_config::BimanualWidowXRobotConfig& config);
        void connect() override;
        void disconnect() override;
        void calibrate() override;
        void configure() override;
        std::string name() const override { return name_; }
        trossen_ai_robot_devices::State get_observation() override;
        void send_action(const std::vector<double>& action) override;
        std::vector<std::string> get_joint_features() const override;
        std::vector<std::string> get_observation_features() const override;
        std::vector<std::pair<std::string, std::string>> get_camera_names() const override;
    private:
        std::string name_;
        std::string left_ip_address_;
        std::string right_ip_address_;
        std::unique_ptr<trossen_ai_robot_devices::TrossenAIArm> right_robot_driver_;
        std::unique_ptr<trossen_ai_robot_devices::TrossenAIArm> left_robot_driver_;
        bool is_connected_ = false;  // Track connection status
        std::vector<trossen_ai_robot_devices::TrossenAICamera> cameras_;
    };

}



} // namespace trossen_ai_robot_devices

#endif // TROSSEN_AI_ROBOT_HPP