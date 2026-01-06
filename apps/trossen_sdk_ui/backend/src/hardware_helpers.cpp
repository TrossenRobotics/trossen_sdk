#include "backend/hardware_helpers.hpp"
#include <trossen_sdk/hw/camera/opencv_camera_component.hpp>
#include <trossen_sdk/hw/arm/trossen_arm_component.hpp>
#include <trossen_sdk/hw/arm/so101_arm_component.hpp>
#include <trossen_sdk/hw/active_hardware_registry.hpp>
#include <iostream>

namespace trossen {
namespace backend {

// Global status tracking
std::map<std::string, HardwareStatus> g_camera_status;
std::map<std::string, HardwareStatus> g_arm_status;

bool connect_camera(
    const std::string& camera_id,
    const nlohmann::json& config,
    std::string& error) {
    try {
        std::string camera_type = config["type"];

        if (camera_type == "opencv") {
            auto camera =
                std::make_shared<trossen::hw::camera::OpenCvCameraComponent>(
                    camera_id);

            // Configure it with the JSON config
            nlohmann::json cam_config = {
                {"device_index", config["device_index"]},
                {"width", config.value("width", 640)},
                {"height", config.value("height", 480)},
                {"fps", config.value("fps", 30)}
            };
            camera->configure(cam_config);

            // Register in active registry
            trossen::hw::ActiveHardwareRegistry::register_active(
                camera_id, camera);

            g_camera_status[camera_id] = {true, ""};
            std::cout << "Successfully connected OpenCV camera: "
                      << camera_id << std::endl;
            return true;
        } else if (camera_type == "realsense") {
            error = "RealSense cameras are not yet supported in this build";
            g_camera_status[camera_id] = {false, error};
            return false;
        } else {
            error = "Unsupported camera type: " + camera_type;
            g_camera_status[camera_id] = {false, error};
            return false;
        }
    } catch (const std::exception& e) {
        error = std::string("Exception connecting camera: ") + e.what();
        g_camera_status[camera_id] = {false, error};
        return false;
    }
}

bool disconnect_camera(const std::string& camera_id, std::string& error) {
    try {
        // Just mark as disconnected - ActiveHardwareRegistry doesn't have a remove method
        g_camera_status[camera_id] = {false, "Manually disconnected"};
        std::cout << "Disconnected camera: " << camera_id << std::endl;
        return true;
    } catch (const std::exception& e) {
        error = std::string("Exception disconnecting camera: ") + e.what();
        return false;
    }
}

bool connect_arm(
    const std::string& arm_id,
    const nlohmann::json& config,
    std::string& error) {
    try {
        std::string arm_type = config["type"];

        if (arm_type == "so101") {
            // Create the SO101 arm component
            auto arm =
                std::make_shared<trossen::hw::arm::SO101ArmComponent>(arm_id);

            nlohmann::json arm_config = {
                {"end_effector", config.value("end_effector", "follower")},
                {"port", config.value("port", "/dev/ttyUSB0")}
            };
            arm->configure(arm_config);

            trossen::hw::ActiveHardwareRegistry::register_active(arm_id, arm);

            g_arm_status[arm_id] = {true, ""};
            std::cout << "Successfully connected SO101 arm: "
                      << arm_id << std::endl;
            return true;
        }

        if (arm_type == "widowx") {
            // Create the arm component
            auto arm =
                std::make_shared<trossen::hw::arm::TrossenArmComponent>(
                    arm_id);

            // Map end_effector value to full SDK format
            std::string sdk_end_effector;
            std::string stored_end_effector = config.value("end_effector", "follower");
            if (stored_end_effector == "leader") {
                sdk_end_effector = "wxai_v0_leader";
            } else if (stored_end_effector == "follower") {
                sdk_end_effector = "wxai_v0_follower";
            } else {
                // Assume it's already in the correct format
                sdk_end_effector = stored_end_effector;
            }

            nlohmann::json arm_config = {
                {"ip_address", config["serv_ip"]},
                {"model", "wxai_v0"},
                {"end_effector", sdk_end_effector}
            };
            arm->configure(arm_config);

            trossen::hw::ActiveHardwareRegistry::register_active(arm_id, arm);

            g_arm_status[arm_id] = {true, ""};
            std::cout << "Successfully connected WidowX arm: "
                      << arm_id << std::endl;
            return true;
        }

        error = "Unsupported arm type: " + arm_type;
        g_arm_status[arm_id] = {false, error};
        return false;
    } catch (const std::exception& e) {
        error = std::string("Exception connecting arm: ") + e.what();
        g_arm_status[arm_id] = {false, error};
        return false;
    }
}

bool disconnect_arm(const std::string& arm_id, std::string& error) {
    try {
        // Just mark as disconnected
        g_arm_status[arm_id] = {false, "Manually disconnected"};
        std::cout << "Disconnected arm: " << arm_id << std::endl;
        return true;
    } catch (const std::exception& e) {
        error = std::string("Exception disconnecting arm: ") + e.what();
        return false;
    }
}

nlohmann::json get_all_hardware_status() {
    nlohmann::json status;

    for (const auto& [id, cam_status] : g_camera_status) {
        status[id] = {
            {"is_connected", cam_status.is_connected},
            {"error_message", cam_status.error_message}
        };
    }

    for (const auto& [id, arm_status] : g_arm_status) {
        status[id] = {
            {"is_connected", arm_status.is_connected},
            {"error_message", arm_status.error_message}
        };
    }

    return status;
}

}  // namespace backend
}  // namespace trossen
