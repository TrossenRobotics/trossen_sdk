#ifndef TROSSEN_SDK__HW__ARM__SO101_FOLLOWER_HPP
#define TROSSEN_SDK__HW__ARM__SO101_FOLLOWER_HPP

#include "feetech_bus.hpp"
#include <map>
#include <string>
#include <memory>

/**
 * @class SO101Follower
 * @brief Control interface for SO-101 follower arm
 * 
 * This class provides control and communication with an SO-101 follower arm,
 * which receives actions and reports observations through a Feetech serial bus.
 */
class SO101Follower {
public:
    /**
     * Construct an SO101Follower instance.
     *
     * This constructor initializes the follower arm interface with the specified
     * serial port. The connection is not established until connect() is called.
     *
     * @param port Serial port device path (e.g., "/dev/ttyUSB0").
     */
    SO101Follower(const std::string &port);
    
    /**
     * Destroy the SO101Follower instance.
     *
     * This destructor automatically disconnects from the arm if still connected.
     */
    ~SO101Follower();

    /**
     * Establish connection to the SO-101 follower arm.
     *
     * This function opens the serial port and initializes communication with
     * the follower arm servos via the Feetech bus protocol.
     *
     * @return true if connection successful, false otherwise.
     */
    bool connect();
    
    /**
     * Disconnect from the SO-101 follower arm.
     *
     * This function closes the serial port connection and releases resources.
     */
    void disconnect();
    
    /**
     * Check if currently connected to the arm.
     *
     * This function queries the connection state without attempting to communicate
     * with the hardware.
     *
     * @return true if connected, false otherwise.
     */
    bool isConnected() const;

    /**
     * Read current state observations from the follower arm.
     *
     * This function queries all servos and retrieves their current positions
     * and states, returning them as a map indexed by joint name.
     *
     * @return Map of joint names to their current positions/states.
     */
    std::map<std::string, int> getObservation();
    
    /**
     * Send action commands to the follower arm.
     *
     * This function transmits target positions to the servos, commanding them
     * to move to the specified positions.
     *
     * @param action Map of joint names to target positions/commands.
     */
    void sendAction(const std::map<std::string, int> &action);

    /**
     * Perform calibration routine for the follower arm.
     *
     * This function executes a calibration sequence to establish zero positions
     * and verify servo functionality.
     */
    void calibrate();
    
    /**
     * Configure the follower arm with default settings.
     *
     * This function applies standard configuration parameters to all servos,
     * including PID gains, speed limits, and torque settings.
     */
    void configure();

private:
    std::unique_ptr<FeetechBus> bus_;
};

#endif // TROSSEN_SDK__HW__ARM__SO101_FOLLOWER_HPP
