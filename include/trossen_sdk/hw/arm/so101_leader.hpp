#ifndef TROSSEN_SDK__HW__ARM__SO101_LEADER_HPP
#define TROSSEN_SDK__HW__ARM__SO101_LEADER_HPP

#include "feetech_bus.hpp"
#include <map>
#include <string>
#include <memory>
#include <vector>

/**
 * @brief SO101 Leader arm driver for teleoperation.
 *
 * This class provides an interface to read joint positions from an SO101 leader arm
 * equipped with Feetech servos. The leader arm is typically used to capture
 * human operator motions for teleoperation tasks.
 */
class SO101Leader {
public:
    /**
     * @brief Construct a new SO101Leader instance.
     *
     * Initializes the leader arm driver with the specified serial port.
     *
     * @param port Serial port device path (e.g., "/dev/ttyUSB0").
     */
    SO101Leader(const std::string &port);
    
    /**
     * @brief Destructor.
     *
     * Automatically disconnects from the arm if still connected.
     */
    ~SO101Leader();

    /**
     * @brief Connect to the leader arm.
     *
     * Establishes a serial connection to the arm and initializes communication
     * with the servo motors.
     *
     * @return true if connection was successful, false otherwise.
     */
    bool connect();
    
    /**
     * @brief Disconnect from the leader arm.
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
     * @brief Read current joint positions from the leader arm.
     *
     * Queries all servo motors and returns their current positions in servo units.
     *
     * @return Map of joint names to position values in servo units.
     */
    std::map<std::string, int> getJointPositions();
    
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

#endif // TROSSEN_SDK__HW__ARM__SO101_LEADER_HPP
