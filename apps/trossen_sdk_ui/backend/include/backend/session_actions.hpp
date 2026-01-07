/**
 * @file session_actions.hpp
 * @brief Recording session setup and teleoperation control
 *
 * Provides session setup functions for different teleoperation modes (SO101, WidowX)
 * and manages the lifecycle of recording sessions including episode management,
 * hardware control loops, and arm reset functionality.
 */

#ifndef SESSION_ACTIONS_HPP
#define SESSION_ACTIONS_HPP

#include <string>
#include <memory>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include "trossen_sdk/runtime/session_manager.hpp"

// Forward declarations
namespace trossen_arm {
class TrossenArmDriver;
}

class SO101ArmDriver;

namespace trossen::backend {

/**
 * @brief Session action types
 *
 * Defines supported teleoperation modes for recording sessions.
 * Each mode requires specific hardware configuration and setup logic.
 */
enum class SessionAction {
    TELEOP_SO101,           ///< SO101 leader-follower teleoperation (single pair)
    TELEOP_WIDOWX,          ///< WidowX leader-follower teleoperation (single pair)
    TELEOP_WIDOWX_BIMANUAL  ///< WidowX bimanual teleoperation (two leader-follower pairs)
};

/**
 * @brief Active recording session state
 *
 * Maintains all state for an active recording session including hardware drivers,
 * session configuration, episode tracking, and control thread handles.
 * Shared between main thread and teleoperation/episode manager threads.
 */
struct ActiveSession {
    std::shared_ptr<trossen::runtime::SessionManager> manager;  ///< Session manager
    std::vector<std::shared_ptr<SO101ArmDriver>> arm_drivers;  ///< SO101 drivers
    std::vector<std::shared_ptr<trossen_arm::TrossenArmDriver>> widowx_drivers;
        ///< WidowX arm drivers (if used)
    std::string session_id;     ///< Unique session identifier
    std::string session_name;   ///< User-friendly session name
    std::string system_id;      ///< Hardware system ID being used
    SessionAction action;       ///< Type of teleoperation action
    int max_episodes;           ///< Total number of episodes to record
    double episode_duration;    ///< Duration of each episode in seconds
    std::chrono::steady_clock::time_point session_start_time;
        ///< Session start timestamp
    std::atomic<bool> all_episodes_complete{false};
        ///< True when all episodes finished
    std::atomic<bool> waiting_for_next{false};
        ///< True when waiting for manual episode progression
    std::atomic<bool> episode_manager_active{false};
        ///< True while episode manager thread is running
    bool teleop_active{false};  ///< True while teleoperation control loop should run
    std::thread teleop_thread;          ///< Thread running teleoperation control loop
    std::thread episode_manager_thread;  ///< Thread managing episode lifecycle
};

/**
 * @brief Convert action string to enum
 *
 * @param action_str Action string ("TELEOP_SO101", "TELEOP_WIDOWX", "TELEOP_WIDOWX_BIMANUAL")
 * @return SessionAction enum value
 * @throws std::invalid_argument if action_str is not recognized
 */
SessionAction string_to_action(const std::string& action_str);

/**
 * @brief Convert action enum to string
 *
 * @param action SessionAction enum value
 * @return Action string representation
 */
std::string action_to_string(SessionAction action);

/**
 * @brief Validate hardware configuration for session action
 *
 * Checks that required hardware (arms, cameras) exists and is properly configured
 * for the requested session action type.
 *
 * @param action Session action type to validate
 * @param system_id Hardware system ID to check
 * @param error Output parameter for error message if validation fails
 * @return true if hardware is valid for action, false otherwise
 */
bool validate_hardware_for_action(
    SessionAction action,
    const std::string& system_id,
    std::string& error);

/**
 * @brief Setup SO101 teleoperation session
 *
 * Initializes SO101 leader and follower arms, creates teleoperation producer,
 * adds camera producers, and spawns control threads.
 *
 * Control loop reads leader arm positions and writes to follower arm.
 * Supports freezing arms between episodes and reset to sleep position after completion.
 *
 * @param active_session Shared pointer to session state (modified in-place)
 * @param system_id Hardware system ID containing SO101 producers
 * @param error Output parameter for error message if setup fails
 * @return true if setup successful, false otherwise
 */
bool setup_so101_teleop(
    std::shared_ptr<ActiveSession> active_session,
    const std::string& system_id,
    std::string& error);

/**
 * @brief Setup WidowX single-pair teleoperation session
 *
 * Initializes WidowX leader and follower arms, creates teleoperation producer,
 * adds camera producers, and spawns control threads.
 *
 * Control loop reads leader arm positions and writes to follower arm.
 * Arms are staged to ready position before starting, frozen between episodes,
 * and reset to sleep position after all episodes complete.
 *
 * @param active_session Shared pointer to session state (modified in-place)
 * @param system_id Hardware system ID containing WidowX producers
 * @param error Output parameter for error message if setup fails
 * @return true if setup successful, false otherwise
 */
bool setup_widowx_teleop(
    std::shared_ptr<ActiveSession> active_session,
    const std::string& system_id,
    std::string& error);

/**
 * @brief Setup WidowX bimanual teleoperation session
 *
 * Initializes two pairs of WidowX leader-follower arms (4 arms total),
 * creates two teleoperation producers, adds camera producers, and spawns control threads.
 *
 * Control loop reads both leader arm positions and writes to corresponding follower arms.
 * All arms are staged to ready position before starting, frozen between episodes,
 * and reset to sleep position after all episodes complete.
 *
 * @param active_session Shared pointer to session state (modified in-place)
 * @param system_id Hardware system ID containing bimanual WidowX producers
 * @param error Output parameter for error message if setup fails
 * @return true if setup successful, false otherwise
 */
bool setup_widowx_bimanual_teleop(
    std::shared_ptr<ActiveSession> active_session,
    const std::string& system_id,
    std::string& error);

/**
 * @brief Setup camera-only recording session (future use)
 *
 * Initializes camera producers without teleoperation. For recording
 * demonstrations without arm control.
 *
 * @param active_session Shared pointer to session state (modified in-place)
 * @param system_id Hardware system ID containing camera producers
 * @param error Output parameter for error message if setup fails
 * @return true if setup successful, false otherwise
 */
bool setup_camera_recording(
    std::shared_ptr<ActiveSession> active_session,
    const std::string& system_id,
    std::string& error);

}  // namespace trossen::backend

#endif  // SESSION_ACTIONS_HPP
