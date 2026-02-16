/**
 * @file hardware_helpers.hpp
 * @brief Hardware connection and status management
 *
 * Provides functions for connecting/disconnecting hardware devices (cameras, arms)
 * and tracking their connection status. Used by REST API endpoints to manage
 * physical hardware lifecycle.
 */

#ifndef HARDWARE_HELPERS_HPP
#define HARDWARE_HELPERS_HPP

#include <string>
#include <map>
#include <nlohmann/json.hpp>

namespace trossen {
namespace backend {

/**
 * @brief Hardware connection status
 *
 * Tracks whether a hardware device is currently connected and stores
 * any error message from the last connection attempt.
 */
struct HardwareStatus {
    bool is_connected;        ///< True if device is currently connected
    std::string error_message;  ///< Error message from last connection attempt (empty if no error)
};

/**
 * @brief Global camera status map
 *
 * Maps camera ID to connection status. Updated by connect/disconnect functions.
 * Keyed by camera ID from configuration.
 */
extern std::map<std::string, HardwareStatus> g_camera_status;

/**
 * @brief Global arm status map
 *
 * Maps arm ID to connection status. Updated by connect/disconnect functions.
 * Keyed by arm ID from configuration.
 */
extern std::map<std::string, HardwareStatus> g_arm_status;

// Camera connection functions
/**
 * @brief Connect to a camera device
 *
 * Initializes camera hardware based on configuration type (OpenCV or RealSense).
 * Performs test capture to verify device is accessible and functional.
 *
 * @param camera_id Unique camera identifier from configuration
 * @param config JSON configuration object containing camera parameters
 * @param error Output parameter for error message if connection fails
 * @return true if connection successful, false otherwise
 */
bool connect_camera(
    const std::string& camera_id,
    const nlohmann::json& config,
    std::string& error);

/**
 * @brief Disconnect from a camera device
 *
 * Releases camera resources and updates connection status.
 *
 * @param camera_id Unique camera identifier from configuration
 * @param error Output parameter for error message if disconnection fails
 * @return true if disconnection successful, false otherwise
 */
bool disconnect_camera(const std::string& camera_id, std::string& error);

// Arm connection functions
/**
 * @brief Connect to a robot arm
 *
 * Initializes arm driver based on configuration type (SO101 or WidowX).
 * Establishes serial or network connection and verifies communication.
 *
 * @param arm_id Unique arm identifier from configuration
 * @param config JSON configuration object containing arm parameters
 * @param error Output parameter for error message if connection fails
 * @return true if connection successful, false otherwise
 */
bool connect_arm(
    const std::string& arm_id,
    const nlohmann::json& config,
    std::string& error);

/**
 * @brief Disconnect from a robot arm
 *
 * Releases arm driver resources and updates connection status.
 * Does not move arm to safe position - caller responsible for arm state.
 *
 * @param arm_id Unique arm identifier from configuration
 * @param error Output parameter for error message if disconnection fails
 * @return true if disconnection successful, false otherwise
 */
bool disconnect_arm(const std::string& arm_id, std::string& error);

/**
 * @brief Get connection status for all hardware
 *
 * Aggregates camera and arm connection status into a single JSON object.
 * Used by frontend to display hardware status.
 *
 * @return JSON object mapping hardware names to status objects
 */
nlohmann::json get_all_hardware_status();

}  // namespace backend
}  // namespace trossen

#endif  // HARDWARE_HELPERS_HPP
