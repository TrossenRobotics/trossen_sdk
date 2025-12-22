#ifndef TROSSEN_SDK__HW__ARM__SO101_ARM_DRIVER_HPP
#define TROSSEN_SDK__HW__ARM__SO101_ARM_DRIVER_HPP

#include "feetech_bus.hpp"
#include <string>
#include <memory>
#include <vector>

/**
 * @brief End effector configuration for SO101 arm
 */
enum class SO101EndEffector {
  leader,    ///< Leader arm for reading operator input
  follower   ///< Follower arm for executing commands
};

/**
 * @brief SO101 arm drivers for the trossen SDK
 *
 * This class provides an interface for SO101 arms that can be configured
 * as either leader or follower roles. The driver communicates with Feetech servos
 * via serial connection.
 */
class SO101ArmDriver {
public:
  /**
   * @brief Default constructor.
   */
  SO101ArmDriver() = default;

  /**
   * @brief Destructor.
   *
   * Automatically disconnects from the arm if still connected.
   */
  ~SO101ArmDriver();

  /**
   * @brief Configure the arm driver.
   *
   * Initializes the arm with the specified end effector type and serial port.
   *
   * @param end_effector End effector type (leader or follower)
   * @param port Serial port device path (e.g., "/dev/ttyUSB0")
   * @return true if configuration was successful, false otherwise
   */
  bool configure(SO101EndEffector end_effector, const std::string& port);

  /**
   * @brief Connect to the arm.
   *
   * Establishes a serial connection to the arm and initializes communication
   * with the servo motors.
   *
   * @return true if connection was successful, false otherwise
   */
  bool connect();

  /**
   * @brief Disconnect from the arm.
   *
   * Closes the serial connection to the arm.
   */
  void disconnect();

  /**
   * @brief Check if the arm is connected.
   *
   * @return true if the arm is currently connected, false otherwise
   */
  bool is_connected() const;

  /**
   * @brief Get current joint positions from the arm.
   *
   * Queries all servo motors and returns their current positions in the order
   * defined by get_joint_names(). Position values are converted from servo units
   * (0-4095 for STS3215) to doubles. If normalization is enabled, values are
   * mapped to [-100, 100] range for dataset storage.
   *
   * @param normalize If true, normalize positions to [-100, 100] range (default: true)
   * @return Vector of joint positions in order matching get_joint_names()
   */
  std::vector<double> get_joint_positions(bool normalize = true);

  /**
   * @brief Set target joint positions for the arm.
   *
   * Commands the servo motors to move to the specified target positions.
   * Positions are expected in the order defined by get_joint_names().
   * If normalization is enabled, values are expected in [-100, 100] range
   * and will be mapped to raw servo units. This function is primarily used
   * for follower arms.
   *
   * @param positions Vector of joint positions in order matching get_joint_names()
   * @param normalize If true, positions are in [-100, 100] range (default: true)
   */
  void set_joint_positions(const std::vector<double>& positions, bool normalize = true);

  /**
   * @brief Get ordered list of joint names.
   *
   * Returns the joint names in a consistent order matching the physical
   * kinematic chain of the arm.
   *
   * @return Vector of joint names in order: shoulder_pan, shoulder_lift,
   *         elbow_flex, wrist_flex, wrist_roll, gripper
   */
  std::vector<std::string> get_joint_names() const;

  /**
   * @brief Get the number of joints.
   *
   * @return Number of joints in the arm (6 for SO101)
   */
  size_t get_num_joints() const;

  /**
   * @brief Get the arm end effector type.
   *
   * @return Current end effector type (leader or follower)
   */
  SO101EndEffector get_end_effector() const { return end_effector_; }

  /**
   * @brief Get the arm model name.
   *
   * @return The model name "SO101"
   */
  std::string get_model_name() const { return "SO101"; }

private:
  /**
   * @brief Calibration range for a joint
   */
  struct JointCalibration {
    int range_min;  ///< Minimum raw encoder value (servo units)
    int range_max;  ///< Maximum raw encoder value (servo units)
  };

  /**
   * @brief Normalize raw servo position to [-100, 100] range
   *
   * @param raw_value Raw servo position value (int servo units)
   * @param calibration Calibration range for the joint
   * @return Normalized value in [-100, 100] range (double)
   */
  double normalize(int raw_value, const JointCalibration& calibration) const;

  /**
   * @brief Unnormalize position from [-100, 100] range to raw servo units
   *
   * @param normalized_value Normalized position in [-100, 100] range (double)
   * @param calibration Calibration range for the joint
   * @return Raw servo position value (int servo units)
   */
  int unnormalize(double normalized_value, const JointCalibration& calibration) const;

  std::unique_ptr<FeetechBus> bus_;
  std::vector<std::string> joint_names_;
  std::vector<JointCalibration> joint_calibrations_;
  SO101EndEffector end_effector_;
  bool configured_{false};
};

#endif  // TROSSEN_SDK__HW__ARM__SO101_ARM_DRIVER_HPP
