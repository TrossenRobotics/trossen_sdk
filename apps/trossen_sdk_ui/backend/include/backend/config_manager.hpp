#ifndef CONFIG_MANAGER_HPP
#define CONFIG_MANAGER_HPP

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <mutex>

namespace trossen::config {

// Camera configuration structures
struct CameraConfig {
    std::string id;    // Unique identifier (also used as stream_id)
    std::string type;  // "opencv" or "realsense"
    std::string name;  // User-friendly name
    int device_index;
    std::string encoding;
    int width;
    int height;
    int fps;
    bool use_device_time;

    // RealSense specific (for future)
    bool enable_depth;

    nlohmann::json to_json() const;
    static CameraConfig from_json(const nlohmann::json& j);
};

// Arm configuration structures
struct ArmConfig {
    std::string id;    // Unique identifier
    std::string type;  // "so101", "wxai_v0", "vxai_v0_right", "vxai_v0_left"
    std::string name;  // User-friendly name

    // SO101 specific
    std::string end_effector;  // "leader" or "follower"
    std::string port;  // Serial port for SO101

    // WidowX specific
    std::string model;  // "wxai_v0", "vxai_v0_right", "vxai_v0_left"
    std::string serv_ip;  // IP address for WidowX
    std::string gripper_variant;  // "base", "leader", "follower"
    bool clear_error;

    nlohmann::json to_json() const;
    static ArmConfig from_json(const nlohmann::json& j);
};

// Hardware system configuration
struct HardwareSystem {
    std::string id;
    std::string name;
    std::vector<std::string> producers;  // Producer IDs

    nlohmann::json to_json() const;
    static HardwareSystem from_json(const nlohmann::json& j);
};

// Producer configuration
struct ProducerConfig {
    std::string id;    // Unique identifier (also used as stream_id)
    std::string name;  // Display name for frontend
    std::string type;  // Producer type
    std::string leader_id;  // For teleop producers (references ArmConfig.id)
    std::string follower_id;  // For teleop producers (references ArmConfig.id)
    std::string camera_id;  // For camera producers (references CameraConfig.id)
    std::string arm_id;  // For arm producers (references ArmConfig.id)
    bool use_device_time;
    bool enforce_requested_fps;
    double warmup_seconds;

    nlohmann::json to_json() const;
    static ProducerConfig from_json(const nlohmann::json& j);
};

// Recording session configuration
struct RecordingSession {
    std::string id;
    std::string name;
    std::vector<std::string> cameras;  // Camera names
    std::vector<std::string> robots;   // Robot names
    std::string system_id;  // Hardware system ID
    std::string action;  // Session action
    int num_episodes;
    double episode_duration;  // Duration in seconds
    std::string backend_type;  // Backend type: "trossen_mcap" or "lerobot_v2"

    nlohmann::json to_json() const;
    static RecordingSession from_json(const nlohmann::json& j);
};

// Activity log entry
struct ActivityLog {
    std::string timestamp;  // Epoch milliseconds as string
    std::string session_id;
    std::string session_name;
    std::string event_type;
    std::string description;

    nlohmann::json to_json() const;
    static ActivityLog from_json(const nlohmann::json& j);
};

// Configuration collection
struct Configurations {
    std::vector<CameraConfig> cameras;
    std::vector<ArmConfig> arms;
    std::vector<HardwareSystem> systems;
    std::vector<ProducerConfig> producers;
    std::vector<RecordingSession> sessions;
    std::vector<ActivityLog> activities;

    nlohmann::json to_json() const;
    static Configurations from_json(const nlohmann::json& j);
};

// Configuration manager class
class ConfigManager {
public:
    explicit ConfigManager(const std::string& config_file);

    // Get current configuration
    Configurations get_configurations();

    // Camera management
    bool validate_camera_config(const CameraConfig& config,
                                std::string& error);
    bool validate_opencv_camera(const CameraConfig& config,
                                std::string& error);
    bool add_camera_config(const CameraConfig& config, std::string& error);
    bool update_camera_config(int index, const CameraConfig& config,
                             std::string& error);
    bool delete_camera_config(int index, std::string& error);

    // Arm management
    bool validate_arm_config(const ArmConfig& config, std::string& error);
    bool validate_so101_arm(const ArmConfig& config, std::string& error);
    bool validate_widowx_arm(const ArmConfig& config, std::string& error);
    bool add_arm_config(const ArmConfig& config, std::string& error);
    bool update_arm_config(int index, const ArmConfig& config,
                          std::string& error);
    bool delete_arm_config(int index, std::string& error);

    // System management
    bool add_system(const HardwareSystem& system, std::string& error);
    bool update_system(const std::string& id, const HardwareSystem& system,
                      std::string& error);
    bool delete_system(const std::string& id, std::string& error);

    // Session management
    bool add_session(const RecordingSession& session, std::string& error);
    bool update_session(const std::string& id,
                       const RecordingSession& session,
                       std::string& error);
    bool delete_session(const std::string& id, std::string& error);

    // Activity logging
    void log_activity(const std::string& session_id,
                     const std::string& session_name,
                     const std::string& event_type,
                     const std::string& description);
    void clear_activities();
    std::vector<ActivityLog> get_recent_activities(int limit = 10);

    // File operations
    bool save_to_file();
    bool load_from_file();
    bool save_raw_json(const nlohmann::json& data);

private:
    std::string config_file_;
    std::string data_file_;
    Configurations configs_;
    std::mutex mutex_;
};

}  // namespace trossen::config

#endif  // CONFIG_MANAGER_HPP
