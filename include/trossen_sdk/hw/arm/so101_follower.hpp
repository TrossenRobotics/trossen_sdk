#ifndef TROSSEN_SDK__HW__ARM__SO101_FOLLOWER_HPP
#define TROSSEN_SDK__HW__ARM__SO101_FOLLOWER_HPP

#include "feetech_bus.hpp"
#include <map>
#include <string>
#include <memory>

class SO101Follower {
public:
    SO101Follower(const std::string &port);
    ~SO101Follower();

    bool connect();
    void disconnect();
    bool isConnected() const;

    std::map<std::string, int> getObservation();
    void sendAction(const std::map<std::string, int> &action);

    void calibrate();
    void configure();

private:
    std::unique_ptr<FeetechBus> bus_;
};

#endif // TROSSEN_SDK__HW__ARM__SO101_FOLLOWER_HPP
