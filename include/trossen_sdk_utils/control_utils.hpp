#ifndef TROSSEN_SDK_UTILS_CONTROL_UTILS_HPP
#define TROSSEN_SDK_UTILS_CONTROL_UTILS_HPP

#include <string>
#include <vector>
#include "trossen_dataset/dataset.hpp"
#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_sdk_utils/logging_utils.hpp"
#include "trossen_ai_robot_devices/trossen_ai_cameras.hpp"
#include <filesystem>
namespace trossen_sdk {

class ControlUtils {
public:
    // Function to write joint positions to the robot arm
    void control_loop(std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot> robot,
                    std::shared_ptr<trossen_ai_robot_devices::teleoperator::TrossenLeader> teleop_robot,
                    float control_time,
                    trossen_dataset::TrossenAIDataset& dataset,
                    bool teleop_mode = false);

    inline void busy_wait_until(const std::chrono::steady_clock::time_point& loop_start, double frequency) {
        auto desired_duration = std::chrono::duration<double>(1.0 / frequency);
        auto elapsed = std::chrono::steady_clock::now() - loop_start;
        auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(desired_duration - elapsed);
        if (remaining.count() > 0) {
            std::this_thread::sleep_for(remaining);
        }
    }

    void display_images(const std::vector<trossen_ai_robot_devices::ImageData>& images) const;

private:
    // TODO: Implement logging utility
};

} // namespace trossen_sdk


#endif // TROSSEN_SDK_UTILS_CONTROL_UTILS_HPP