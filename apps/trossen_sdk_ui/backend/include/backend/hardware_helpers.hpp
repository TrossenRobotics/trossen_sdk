#ifndef HARDWARE_HELPERS_HPP
#define HARDWARE_HELPERS_HPP

#include <string>
#include <map>
#include <nlohmann/json.hpp>

namespace trossen {
namespace backend {

// Hardware status tracking
struct HardwareStatus {
    bool is_connected;
    std::string error_message;
};

// Global hardware status maps
extern std::map<std::string, HardwareStatus> g_camera_status;
extern std::map<std::string, HardwareStatus> g_arm_status;

// Camera connection functions
bool connect_camera(
    const std::string& camera_id,
    const nlohmann::json& config,
    std::string& error);

bool disconnect_camera(const std::string& camera_id, std::string& error);

// Arm connection functions
bool connect_arm(
    const std::string& arm_id,
    const nlohmann::json& config,
    std::string& error);

bool disconnect_arm(const std::string& arm_id, std::string& error);

// Get all hardware status
nlohmann::json get_all_hardware_status();

}  // namespace backend
}  // namespace trossen

#endif  // HARDWARE_HELPERS_HPP
