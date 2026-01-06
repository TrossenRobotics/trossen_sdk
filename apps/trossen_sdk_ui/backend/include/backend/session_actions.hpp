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

// Session action types
enum class SessionAction {
    TELEOP_SO101,
    TELEOP_WIDOWX,
    TELEOP_WIDOWX_BIMANUAL
};

// Active session state
struct ActiveSession {
    std::shared_ptr<trossen::runtime::SessionManager> manager;
    std::vector<std::shared_ptr<SO101ArmDriver>> arm_drivers;
    std::vector<std::shared_ptr<trossen_arm::TrossenArmDriver>> widowx_drivers;
    std::string session_id;
    std::string session_name;
    std::string system_id;
    SessionAction action;
    int max_episodes;
    double episode_duration;
    std::chrono::steady_clock::time_point session_start_time;
    std::atomic<bool> all_episodes_complete{false};
    std::atomic<bool> waiting_for_next{false};
    std::atomic<bool> episode_manager_active{false};
    bool teleop_active{false};
    std::thread teleop_thread;
    std::thread episode_manager_thread;
};

// Action conversion functions
SessionAction string_to_action(const std::string& action_str);
std::string action_to_string(SessionAction action);

// Validation function
bool validate_hardware_for_action(
    SessionAction action,
    const std::string& system_id,
    std::string& error);

// Setup functions for each action type
bool setup_so101_teleop(
    std::shared_ptr<ActiveSession> active_session,
    const std::string& system_id,
    std::string& error);

bool setup_widowx_teleop(
    std::shared_ptr<ActiveSession> active_session,
    const std::string& system_id,
    std::string& error);

bool setup_widowx_bimanual_teleop(
    std::shared_ptr<ActiveSession> active_session,
    const std::string& system_id,
    std::string& error);

bool setup_camera_recording(
    std::shared_ptr<ActiveSession> active_session,
    const std::string& system_id,
    std::string& error);

}  // namespace trossen::backend

#endif  // SESSION_ACTIONS_HPP
