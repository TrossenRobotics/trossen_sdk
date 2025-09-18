#ifndef TROSSEN_AI_DRIVER_HPP
#define TROSSEN_AI_DRIVER_HPP
#include <iostream>
#include <string>
#include <vector>
#include "libtrossen_arm/trossen_arm.hpp"
#include <spdlog/spdlog.h>
#include "trossen_sdk_utils/constants.hpp"


namespace trossen_ai_robot_devices {
    /**
     * @brief Class representing a Trossen AI Robotic Arm
     * This is used as a wrapper around the TrossenArmDriver from the libtrossen_arm library
     * to provide a simplified interface for connecting, disconnecting, reading, writing,
     * and staging the robotic arm.
     */
    class TrossenAIArm {
    public:

        /**
         * @brief Constructor for TrossenAIArm
         * @param name Name of the robotic arm
         * @param ip_address IP address of the robotic arm
         * @param model Model of the robotic arm (e.g., "leader", "follower")
         * Initializes the TrossenAIArm with the given parameters
         * 
         */
        TrossenAIArm(const std::string& name, const std::string& ip_address, const std::string& model);
        
        /**
         * @brief Move constructor and move assignment operator
         * Defaulted to allow moving of TrossenAIArm instances
         * This is safe and efficient as it transfers ownership of resources         
         */
        TrossenAIArm(TrossenAIArm&&) noexcept = default;
        TrossenAIArm& operator=(TrossenAIArm&&) noexcept = default;

        /**
         * @brief Copy constructor and copy assignment operator
         * Deleted to prevent copying of TrossenAIArm instances
         * This is to ensure unique ownership of the underlying driver resources like
         * ROS nodes, hardware pointers, file handles, std::unique_ptr, etc.
         * which cannot be shared or duplicated safely.
         * Copying would result in double deletion or undefined behavior.
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
         * @param data_name Name of the data to read (e.g., trossen_sdk::POSITION, "trossen_sdk::VELOCITY", trossen_sdk::EXTERNAL_EFFORT)
         * @return Vector of doubles containing the requested data
         * Reads the specified data from the arm and returns it as a vector of doubles
         */
        std::vector<double> read(std::string data_name);

        /**
         * @brief Write data to the robotic arm
         * @param data_name Name of the data to write (e.g., trossen_sdk::POSITION, "trossen_sdk::VELOCITY", trossen_sdk::EXTERNAL_EFFORT)
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

        //TODO [TDS-32] Make time_to_move_ configurable
        /// @brief Time to move for position/velocity/effort commands
        float time_to_move_ = 0.1;
    };

} // namespace trossen_ai_robot_devices


#endif // TROSSEN_AI_DRIVER_HPP