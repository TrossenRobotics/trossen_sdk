#include "trossen_sdk/hw/arm/so101_follower.hpp"
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
    
    joint_names_ = {
        "shoulder_pan",
        "shoulder_lift",
        "elbow_flex",
        "wrist_flex",
        "wrist_roll",
        "gripper"
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

std::map<std::string, int> SO101Follower::getJointPositions() {
    return bus_->syncReadPosition();
}

void SO101Follower::setJointPositions(const std::map<std::string, int> &positions) {
    bus_->syncWritePosition(positions);
}

std::vector<std::string> SO101Follower::getJointNames() const {
    return joint_names_;
}
