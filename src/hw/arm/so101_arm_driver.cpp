/**
 * @file so101_arm_driver.cpp
 * @brief Implementation of unified SO101 arm driver.
 */

#include "trossen_sdk/hw/arm/so101_arm_driver.hpp"
#include <iostream>
#include <vector>
#include <string>

SO101ArmDriver::~SO101ArmDriver() {
  disconnect();
}

bool SO101ArmDriver::configure(SO101EndEffector end_effector, const std::string& port) {
  if (configured_) {
    std::cerr << "SO101ArmDriver: Already configured" << std::endl;
    return false;
  }

  // Store configuration
  end_effector_ = end_effector;

  // Define motor configuration in order
  std::vector<Motor> motors = {
    {1, "sts3215", 0, 100},  // shoulder_pan
    {2, "sts3215", 0, 100},  // shoulder_lift
    {3, "sts3215", 0, 100},  // elbow_flex
    {4, "sts3215", 0, 100},  // wrist_flex
    {5, "sts3215", 0, 100},  // wrist_roll
    {6, "sts3215", 0, 100}   // gripper
  };

  // Define joint names in same order as motors
  joint_names_ = {
    "shoulder_pan",
    "shoulder_lift",
    "elbow_flex",
    "wrist_flex",
    "wrist_roll",
    "gripper"
  };

  // Define calibration ranges for each joint (min/max raw encoder values)
  // These values should be calibrated based on actual mechanical limits
  // Currently using reasonable defaults for STS3215 servos (0-4095 range)
  joint_calibrations_ = {
    {0, 4095},    // shoulder_pan
    {0, 4095},    // shoulder_lift
    {0, 4095},    // elbow_flex
    {0, 4095},    // wrist_flex
    {0, 4095},    // wrist_roll
    {0, 4095}     // gripper
  };

  // Initialize Feetech bus
  bus_ = std::make_unique<FeetechBus>(port, motors);

  configured_ = true;
  return true;
}

bool SO101ArmDriver::connect() {
  if (!configured_) {
    std::cerr << "SO101ArmDriver: Must call configure() before connect()" << std::endl;
    return false;
  }

  if (bus_->is_connected()) {
    return true;
  }

  return bus_->connect();
}

void SO101ArmDriver::disconnect() {
  if (bus_) {
    bus_->disconnect();
  }
}

bool SO101ArmDriver::is_connected() const {
  if (!bus_) {
    return false;
  }
  return bus_->is_connected();
}

std::vector<double> SO101ArmDriver::get_joint_positions(bool normalize) {
  if (!bus_) {
    std::cerr << "SO101ArmDriver: Not configured" << std::endl;
    return {};
  }

  // Read positions from bus (returns vector of ints in motor order)
  std::vector<int> raw_ints = bus_->sync_read_position();

  std::vector<double> positions;
  positions.reserve(raw_ints.size());

  // Apply normalization if requested
  if (normalize) {
    for (size_t i = 0; i < raw_ints.size(); ++i) {
      positions.push_back(this->normalize(raw_ints[i], joint_calibrations_[i]));
    }
  } else {
    // Return raw values as doubles (no normalization)
    for (int raw_int : raw_ints) {
      positions.push_back(static_cast<double>(raw_int));
    }
  }

  return positions;
}

void SO101ArmDriver::set_joint_positions(const std::vector<double>& positions, bool normalize) {
  if (!bus_) {
    std::cerr << "SO101ArmDriver: Not configured" << std::endl;
    return;
  }

  std::vector<int> raw_ints;
  raw_ints.reserve(positions.size());

  // Apply unnormalization if needed
  if (normalize) {
    for (size_t i = 0; i < positions.size(); ++i) {
      raw_ints.push_back(this->unnormalize(positions[i], joint_calibrations_[i]));
    }
  } else {
    // Convert double to int for raw values
    for (double pos : positions) {
      raw_ints.push_back(static_cast<int>(pos));
    }
  }

  bus_->sync_write_position(raw_ints);
}

std::vector<std::string> SO101ArmDriver::get_joint_names() const {
  return joint_names_;
}

size_t SO101ArmDriver::get_num_joints() const {
  return joint_names_.size();
}

double SO101ArmDriver::normalize(int raw_value, const JointCalibration& calibration) const {
  // Map from raw range [min, max] to normalized range [-100, 100]
  // Formula: normalized = (((raw - min) / (max - min)) * 200.0) - 100.0
  int range = calibration.range_max - calibration.range_min;
  if (range == 0) {
    return 0.0;  // Avoid division by zero
  }
  double normalized =
      (((static_cast<double>(raw_value - calibration.range_min)) / range) *
       NORMALIZED_RANGE) +
      NORMALIZED_MIN;
  if (normalized < NORMALIZED_MIN) normalized = NORMALIZED_MIN;
  if (normalized > NORMALIZED_MAX) normalized = NORMALIZED_MAX;
  return normalized;
}

int SO101ArmDriver::unnormalize(
    double normalized_value, const JointCalibration& calibration) const {
  // Map from normalized range to raw range [min, max]
  // Formula: raw = ((normalized - min_norm) / range_norm) * (max - min) + min
  double clamped = normalized_value;
  if (clamped < NORMALIZED_MIN) clamped = NORMALIZED_MIN;
  if (clamped > NORMALIZED_MAX) clamped = NORMALIZED_MAX;

  int range = calibration.range_max - calibration.range_min;
  double raw_double =
      ((clamped - NORMALIZED_MIN) / NORMALIZED_RANGE) * range + calibration.range_min;
  return static_cast<int>(raw_double + 0.5);  // Round to nearest int
}
