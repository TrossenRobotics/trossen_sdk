#ifndef TROSSEN_SDK__HW__ARM__SO101_LEADER_HPP
#define TROSSEN_SDK__HW__ARM__SO101_LEADER_HPP

#include "feetech_bus.hpp"
#include <map>
#include <string>
#include <memory>

class SO101Leader {
public:
    SO101Leader(const std::string &port);
    ~SO101Leader();

    bool connect();
    void disconnect();
    bool isConnected() const;

    std::map<std::string, int> getAction();
    void sendFeedback(const std::map<std::string, int> &feedback);

    void calibrate();
    void configure();

private:
    std::unique_ptr<FeetechBus> bus_;
};

#endif // TROSSEN_SDK__HW__ARM__SO101_LEADER_HPP
