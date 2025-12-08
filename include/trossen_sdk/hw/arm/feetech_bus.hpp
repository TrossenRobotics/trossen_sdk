#ifndef TROSSEN_SDK__HW__ARM__FEETECH_BUS_HPP
#define TROSSEN_SDK__HW__ARM__FEETECH_BUS_HPP

#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <ftservo/ftservo.h>

class SMS_STS;

/**
 * @brief Motor configuration parameters.
 *
 * Defines the properties of a single servo motor in the arm.
 */
struct Motor {
    /// @brief Servo motor ID
    int id;

    /// @brief Motor model name
    std::string model;

    /// @brief Minimum position range in degrees or servo units
    double min_range;

    /// @brief Maximum position range in degrees or servo units
    double max_range;
};

/**
 * @brief Low-level communication bus for Feetech servo motors.
 *
 * This class provides a thread-safe interface to communicate with Feetech
 * STS series servo motors over a serial bus. It handles connection management,
 * position reading, and position writing for multiple servos.
 */
class FeetechBus {
public:
    /**
     * @brief Construct a new FeetechBus instance.
     *
     * Initializes the servo bus with the specified serial port and motor configuration.
     * Motors are identified by name and configured with their ID and range parameters.
     *
     * @param port Serial port device path (e.g., "/dev/ttyUSB0").
     * @param motors Map of motor names to Motor configuration structs.
     */
    FeetechBus(const std::string &port, const std::map<std::string, Motor> &motors);

    /**
     * @brief Destructor.
     *
     * Automatically disconnects from the servo bus if still connected.
     */
    ~FeetechBus();

    /**
     * @brief Connect to the servo bus.
     *
     * Opens the serial port and initializes communication with the servo motors.
     * This must be called before any read or write operations.
     *
     * @return true if connection was successful, false otherwise.
     */
    bool connect();

    /**
     * @brief Disconnect from the servo bus.
     *
     * Closes the serial port connection to the servo motors.
     */
    void disconnect();

    /**
     * @brief Check if the bus is connected.
     *
     * @return true if the bus is currently connected, false otherwise.
     */
    bool isConnected() const;

    /**
     * @brief Read current positions from all motors synchronously.
     *
     * Queries each configured motor for its current position and returns
     * the results as a map. Position values are in servo units.
     *
     * @return Map of motor names to current position values in servo units.
     */
    std::map<std::string, int> syncReadPosition();

    /**
     * @brief Write target positions to motors synchronously.
     *
     * Commands each specified motor to move to its target position.
     * Position values should be in servo units matching the motor's range.
     *
     * @param goal_positions Map of motor names to target position values in servo units.
     */
    void syncWritePosition(const std::map<std::string, int> &goal_positions);

private:
    /// @brief Serial port device path
    std::string port_;

    /// @brief Map of motor names to Motor configurations
    std::map<std::string, Motor> motors_;

    /// @brief Connection status flag
    bool connected_;

    /// @brief Mutex for thread-safe bus access
    std::mutex bus_mutex_;

    /// @brief Pointer to the underlying SMS_STS servo driver
    std::unique_ptr<SMS_STS> servo_;
};

#endif  // TROSSEN_SDK__HW__ARM__FEETECH_BUS_HPP
