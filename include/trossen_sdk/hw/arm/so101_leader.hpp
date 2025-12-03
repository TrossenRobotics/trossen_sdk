#ifndef TROSSEN_SDK__HW__ARM__SO101_LEADER_HPP
#define TROSSEN_SDK__HW__ARM__SO101_LEADER_HPP

#include "feetech_bus.hpp"
#include <map>
#include <string>
#include <memory>

/**
 * @class SO101Leader
 * @brief Control interface for SO-101 leader arm
 * 
 * This class provides control and communication with an SO-101 leader arm,
 * which reads user actions and provides feedback through a Feetech serial bus.
 * Typically used in teleoperation scenarios.
 */
class SO101Leader {
public:
    /**
     * Construct an SO101Leader instance.
     *
     * This constructor initializes the leader arm interface with the specified
     * serial port. The connection is not established until connect() is called.
     *
     * @param port Serial port device path (e.g., "/dev/ttyUSB0").
     */
    SO101Leader(const std::string &port);
    
    /**
     * Destroy the SO101Leader instance.
     *
     * This destructor automatically disconnects from the arm if still connected.
     */
    ~SO101Leader();

    /**
     * Establish connection to the SO-101 leader arm.
     *
     * This function opens the serial port and initializes communication with
     * the leader arm servos via the Feetech bus protocol.
     *
     * @return true if connection successful, false otherwise.
     */
    bool connect();
    
    /**
     * Disconnect from the SO-101 leader arm.
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
     * Read action commands from the leader arm.
     *
     * This function queries all servos to read the operator's commanded positions,
     * returning them as a map indexed by joint name for teleoperation use.
     *
     * @return Map of joint names to their current positions/actions.
     */
    std::map<std::string, int> getAction();
    
    /**
     * Send feedback to the leader arm.
     *
     * This function transmits feedback values (e.g., force feedback or resistance)
     * to the leader arm servos to provide haptic cues to the operator.
     *
     * @param feedback Map of joint names to feedback values.
     */
    void sendFeedback(const std::map<std::string, int> &feedback);

    /**
     * Perform calibration routine for the leader arm.
     *
     * This function executes a calibration sequence to establish zero positions
     * and verify servo functionality.
     */
    void calibrate();
    
    /**
     * Configure the leader arm with default settings.
     *
     * This function applies standard configuration parameters to all servos,
     * including PID gains, speed limits, and torque settings.
     */
    void configure();

private:
    std::unique_ptr<FeetechBus> bus_;
};

#endif // TROSSEN_SDK__HW__ARM__SO101_LEADER_HPP
