#include "so101_drivers/so101_follower.hpp"
#include <iostream>

SO101Follower::SO101Follower(const std::string &port) {
    std::map<std::string, Motor> motors = {
        {"shoulder_pan", {1, "sts3215", 0, 100}},
        {"shoulder_lift", {2, "sts3215", 0, 100}},
        {"elbow_flex", {3, "sts3215", 0, 100}},
        {"wrist_flex", {4, "sts3215", 0, 100}},
        {"wrist_roll", {5, "sts3215", 0, 100}},
        {"gripper", {6, "sts3215", 0, 100}}
    };
    bus_ = std::make_unique<FeetechBus>(port, motors);
}

SO101Follower::~SO101Follower() {
    disconnect();
}

bool SO101Follower::connect() {
    if (bus_->isConnected()) return true;
    return bus_->connect();
}

void SO101Follower::disconnect() {
    bus_->disconnect();
}

bool SO101Follower::isConnected() const {
    return bus_->isConnected();
}

std::map<std::string, int> SO101Follower::getObservation() {
    return bus_->syncReadPosition();
}

void SO101Follower::sendAction(const std::map<std::string, int> &action) {
    bus_->syncWritePosition(action);
}

void SO101Follower::calibrate() {
    // Unlock EPROM to write calibration data
    std::map<std::string, Motor> motors = {
        {"shoulder_pan", {1, "sts3215", 0, 100}},
        {"shoulder_lift", {2, "sts3215", 0, 100}},
        {"elbow_flex", {3, "sts3215", 0, 100}},
        {"wrist_flex", {4, "sts3215", 0, 100}},
        {"wrist_roll", {5, "sts3215", 0, 100}},
        {"gripper", {6, "sts3215", 0, 100}}
    };
    
    // Read current positions and set as calibration offsets
    std::map<std::string, MotorCalibration> calibration;
    auto positions = bus_->syncReadPosition();
    
    for (const auto &[name, motor] : motors) {
        MotorCalibration cal;
        cal.id = motor.id;
        cal.drive_mode = 0; // Position mode
        cal.homing_offset = positions[name];
        cal.range_min = static_cast<int>(motor.min_range);
        cal.range_max = static_cast<int>(motor.max_range);
        calibration[name] = cal;
        
        std::cout << "Calibrated " << name << " at position " << positions[name] << std::endl;
    }
    
    bus_->writeCalibration(calibration);
}

void SO101Follower::configure() {
    bus_->disableTorque();
    bus_->configureMotors();
    bus_->enableTorque();
}
