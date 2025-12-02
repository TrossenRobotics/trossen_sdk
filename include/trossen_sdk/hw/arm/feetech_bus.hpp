#ifndef TROSSEN_SDK__HW__ARM__FEETECH_BUS_HPP
#define TROSSEN_SDK__HW__ARM__FEETECH_BUS_HPP

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <ftservo/ftservo.h>

class SMS_STS;

struct MotorCalibration {
    int id;
    int drive_mode;
    int homing_offset;
    int range_min;
    int range_max;
};

struct Motor {
    int id;
    std::string model;
    double min_range;
    double max_range;
};

class FeetechBus {
public:
    FeetechBus(const std::string &port, const std::map<std::string, Motor> &motors);
    ~FeetechBus();

    bool connect();
    void disconnect();
    bool isConnected() const;

    std::map<std::string, int> syncReadPosition();
    void syncWritePosition(const std::map<std::string, int> &goal_positions);

    void configureMotors();
    void disableTorque();
    void enableTorque();

    void writeCalibration(const std::map<std::string, MotorCalibration> &calibration);
    bool isCalibrated() const;

private:
    std::string port_;
    std::map<std::string, Motor> motors_;
    std::map<int, MotorCalibration> calibration_;
    bool connected_;
    std::mutex bus_mutex_;
    std::unique_ptr<SMS_STS> servo_;

    // Internal helper for FTServo
    void writeMotor(int id, int address, int value);
    int readMotor(int id, int address);
};

#endif // TROSSEN_SDK__HW__ARM__FEETECH_BUS_HPP
