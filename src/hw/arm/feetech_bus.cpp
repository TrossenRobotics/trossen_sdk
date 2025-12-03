#include "trossen_sdk/hw/arm/feetech_bus.hpp"
#include <iostream>

FeetechBus::FeetechBus(const std::string &port, const std::map<std::string, Motor> &motors)
    : port_(port), motors_(motors), connected_(false), servo_(std::make_unique<SMS_STS>()) {}

FeetechBus::~FeetechBus() {
    if (connected_) disconnect();
}

bool FeetechBus::connect() {
    std::lock_guard<std::mutex> lock(bus_mutex_);
    if (connected_) return true;

    if (!servo_->begin(1000000, port_.c_str())) {
        std::cerr << "Failed to init Feetech bus on " << port_ << std::endl;
        return false;
    }
    connected_ = true;
    return true;
}

void FeetechBus::disconnect() {
    std::lock_guard<std::mutex> lock(bus_mutex_);
    if (!connected_) return;
    servo_->end();
    connected_ = false;
}

bool FeetechBus::isConnected() const {
    return connected_;
}

std::map<std::string, int> FeetechBus::syncReadPosition() {
    std::lock_guard<std::mutex> lock(bus_mutex_);
    std::map<std::string, int> positions;
    for (auto &[name, motor] : motors_) {
        int pos = servo_->ReadPos(motor.id);
        positions[name] = pos;
    }
    return positions;
}

void FeetechBus::syncWritePosition(const std::map<std::string, int> &goal_positions) {
    std::lock_guard<std::mutex> lock(bus_mutex_);
    for (auto &[name, goal] : goal_positions) {
        servo_->WritePosEx(motors_[name].id, goal, 0, 0);
    }
}
