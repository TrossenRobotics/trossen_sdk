#ifndef TROSSEN_SDK__HW__ARM__SO101_FOLLOWER_HPP
#define TROSSEN_SDK__HW__ARM__SO101_FOLLOWER_HPP

#include "feetech_bus.hpp"
#include <map>
#include <string>
#include <memory>
#include <vector>

/**
 * @brief SO101 Follower arm driver for teleoperation.
 *
 * This class provides an interface to control an SO101 follower arm equipped
 * with Feetech servos. The follower arm receives position commands
 * and replicates the motions of a leader arm or other control input.
 */
class SO101Follower {
public:
    /**
     * @brief Construct a new SO101Follower instance.
     *
     * Initializes the follower arm driver with the specified serial port.
     *
     * @param port Serial port device path (e.g., "/dev/ttyUSB0").
     */
    SO101Follower(const std::string &port);
    
    /**
     * @brief Destructor.
     *
     * Automatically disconnects from the arm if still connected.
     */
    ~SO101Follower();

    /**
     * @brief Connect to the follower arm.
     *
     * Establishes a serial connection to the arm and initializes communication
     * with the servo motors.
     *
     * @return true if connection was successful, false otherwise.
     */
    bool connect();
    
    /**
     * @brief Disconnect from the follower arm.
     *
     * Closes the serial connection to the arm.
     */
    void disconnect();
    
    /**
     * @brief Check if the arm is connected.
     *
     * @return true if the arm is currently connected, false otherwise.
     */
    bool isConnected() const;

    /**
     * @brief Read current joint positions from the follower arm.
     *
     * Queries all servo motors and returns their current positions in servo units.
     * Position values are typically in the range 0-4095 for STS3215 servos.
     *
     * @return Map of joint names to position values in servo units.
     */
    std::map<std::string, int> getJointPositions();
    
    /**
     * @brief Write target joint positions to the follower arm.
     *
     * Commands the servo motors to move to the specified target positions.
     *
     * @param positions Map of joint names to target position values in servo units.
     */
    void setJointPositions(const std::map<std::string, int> &positions);
    
    /**
     * @brief Get ordered list of joint names.
     *
     * Returns the joint names in a consistent order matching the physical
     * kinematic chain of the arm.
     *
     * @return Vector of joint names in order: shoulder_pan, shoulder_lift,
     *         elbow_flex, wrist_flex, wrist_roll, gripper.
     */
    std::vector<std::string> getJointNames() const;
    
    /**
     * @brief Get the arm model name.
     *
     * @return The model name "SO101".
     */
    std::string getModelName() const { return "SO101"; }

private:
    std::unique_ptr<FeetechBus> bus_;
    std::vector<std::string> joint_names_;
};

#endif // TROSSEN_SDK__HW__ARM__SO101_FOLLOWER_HPP
