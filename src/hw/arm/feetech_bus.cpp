#include "trossen_sdk/hw/arm/feetech_bus.hpp"
#include <iostream>

FeetechBus::FeetechBus(const std::string &port, const std::vector<Motor> &motors)
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

bool FeetechBus::is_connected() const {
    return connected_;
}

std::vector<int> FeetechBus::sync_read_position() {
    std::lock_guard<std::mutex> lock(bus_mutex_);
    std::vector<int> positions;
    positions.reserve(motors_.size());

    for (const auto &motor : motors_) {
        int pos = servo_->ReadPos(motor.id);
        positions.push_back(pos);
    }
    return positions;
}

void FeetechBus::sync_write_position(const std::vector<int> &goal_positions) {
    std::lock_guard<std::mutex> lock(bus_mutex_);

    size_t num_motors = std::min(goal_positions.size(), motors_.size());
    for (size_t i = 0; i < num_motors; ++i) {
        servo_->WritePosEx(motors_[i].id, goal_positions[i], 0, 0);
    }
}
