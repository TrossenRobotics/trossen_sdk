#include "backend/config_manager.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>

// SDK includes for validation
#include "trossen_sdk/hw/camera/opencv_producer.hpp"
#include "trossen_sdk/hw/arm/so101_arm_driver.hpp"

namespace trossen::config {

// CameraConfig JSON conversion
nlohmann::json CameraConfig::to_json() const {
    return {
        {"type", type},
        {"name", name},
        {"device_index", device_index},
        {"stream_id", stream_id},
        {"encoding", encoding},
        {"width", width},
        {"height", height},
        {"fps", fps},
        {"use_device_time", use_device_time},
        {"enable_depth", enable_depth}
    };
}

CameraConfig CameraConfig::from_json(const nlohmann::json& j) {
    CameraConfig config;
    config.type = j.value("type", "opencv");
    config.name = j.value("name", "camera");
    config.device_index = j.value("device_index", 0);
    config.stream_id = j.value("stream_id", "camera0");
    config.encoding = j.value("encoding", "bgr8");
    config.width = j.value("width", 640);
    config.height = j.value("height", 480);
    config.fps = j.value("fps", 30);
    config.use_device_time = j.value("use_device_time", false);
    config.enable_depth = j.value("enable_depth", false);
    return config;
}

// ArmConfig JSON conversion
nlohmann::json ArmConfig::to_json() const {
    nlohmann::json j = {
        {"type", type},
        {"name", name}
    };

    if (type == "so101") {
        j["end_effector"] = end_effector;
        j["port"] = port;
    } else if (type == "widowx") {
        j["end_effector"] = end_effector;
        j["serv_ip"] = serv_ip;
    }

    return j;
}

ArmConfig ArmConfig::from_json(const nlohmann::json& j) {
    ArmConfig config;
    config.type = j.value("type", "");
    config.name = j.value("name", "arm");
    config.end_effector = j.value("end_effector", "");

    if (config.type == "so101") {
        config.port = j.value("port", "");
    } else if (config.type == "widowx") {
        config.serv_ip = j.value("serv_ip", "");
    }

    return config;
}

// HardwareSystem JSON conversion
nlohmann::json HardwareSystem::to_json() const {
    return {
        {"id", id},
        {"name", name},
        {"producers", producers}
    };
}

HardwareSystem HardwareSystem::from_json(const nlohmann::json& j) {
    HardwareSystem system;
    system.id = j.value("id", "");
    system.name = j.value("name", "");
    system.producers = j.value("producers", std::vector<std::string>());
    return system;
}

// ProducerConfig JSON conversion
nlohmann::json ProducerConfig::to_json() const {
    return {
        {"id", stream_id},  // Output id field (using stream_id which holds the producer ID)
        {"stream_id", stream_id},
        {"type", type},
        {"leader_name", leader_name},
        {"follower_name", follower_name},
        {"camera_name", camera_name},
        {"serial_number", serial_number},
        {"device_index", device_index},
        {"width", width},
        {"height", height},
        {"fps", fps},
        {"encoding", encoding},
        {"use_device_time", use_device_time},
        {"enforce_requested_fps", enforce_requested_fps},
        {"warmup_seconds", warmup_seconds}
    };
}

ProducerConfig ProducerConfig::from_json(const nlohmann::json& j) {
    ProducerConfig config;

    // Handle nested config structure from frontend
    const nlohmann::json& cfg = j.contains("config") ? j["config"] : j;

    config.stream_id = j.value("id", cfg.value("stream_id", ""));
    config.type = j.value("type", "");  // Type is at top level
    config.leader_name = cfg.value("leader_name", "");
    config.follower_name = cfg.value("follower_name", "");
    config.camera_name = cfg.value("camera_name", "");
    config.serial_number = cfg.value("serial_number", "");
    config.device_index = cfg.value("device_index", 0);
    config.width = cfg.value("width", 640);
    config.height = cfg.value("height", 480);
    config.fps = cfg.value("fps", 30);
    config.encoding = cfg.value("encoding", "bgr8");
    config.use_device_time = cfg.value("use_device_time", false);
    config.enforce_requested_fps = cfg.value("enforce_requested_fps", true);
    config.warmup_seconds = cfg.value("warmup_seconds", 2.0);
    return config;
}

// RecordingSession JSON conversion
nlohmann::json RecordingSession::to_json() const {
    return {
        {"id", id},
        {"name", name},
        {"cameras", cameras},
        {"robots", robots},
        {"system_id", system_id},
        {"action", action},
        {"num_episodes", num_episodes},
        {"episode_duration", episode_duration},
        {"backend_type", backend_type}
    };
}

RecordingSession RecordingSession::from_json(const nlohmann::json& j) {
    RecordingSession session;
    session.id = j.value("id", "");
    session.name = j.value("name", "");
    session.cameras = j.value("cameras", std::vector<std::string>());
    session.robots = j.value("robots", std::vector<std::string>());
    session.system_id = j.value("system_id", "");
    session.action = j.value("action", "teleop_so101");
    session.num_episodes = j.value("num_episodes", 1);
    session.episode_duration = j.value("episode_duration", 60.0);
    session.backend_type = j.value("backend_type", "mcap");
    return session;
}

// ActivityLog JSON conversion
nlohmann::json ActivityLog::to_json() const {
    return {
        {"timestamp", timestamp},
        {"session_id", session_id},
        {"session_name", session_name},
        {"event_type", event_type},
        {"description", description}
    };
}

ActivityLog ActivityLog::from_json(const nlohmann::json& j) {
    ActivityLog log;
    log.timestamp = j.value("timestamp", "");
    log.session_id = j.value("session_id", "");
    log.session_name = j.value("session_name", "");
    log.event_type = j.value("event_type", "");
    log.description = j.value("description", "");
    return log;
}

// Configurations JSON conversion
nlohmann::json Configurations::to_json() const {
    nlohmann::json cameras_json = nlohmann::json::array();
    for (const auto& cam : cameras) {
        cameras_json.push_back(cam.to_json());
    }

    nlohmann::json arms_json = nlohmann::json::array();
    for (const auto& arm : arms) {
        arms_json.push_back(arm.to_json());
    }

    nlohmann::json systems_json = nlohmann::json::array();
    for (const auto& sys : systems) {
        systems_json.push_back(sys.to_json());
    }

    nlohmann::json producers_json = nlohmann::json::array();
    for (const auto& prod : producers) {
        producers_json.push_back(prod.to_json());
    }

    nlohmann::json sessions_json = nlohmann::json::array();
    for (const auto& session : sessions) {
        sessions_json.push_back(session.to_json());
    }

    nlohmann::json activities_json = nlohmann::json::array();
    for (const auto& activity : activities) {
        activities_json.push_back(activity.to_json());
    }

    return {
        {"cameras", cameras_json},
        {"arms", arms_json},
        {"systems", systems_json},
        {"producers", producers_json},
        {"sessions", sessions_json},
        {"activities", activities_json}
    };
}

Configurations Configurations::from_json(const nlohmann::json& j) {
    Configurations configs;

    if (j.contains("cameras") && j["cameras"].is_array()) {
        for (const auto& cam_json : j["cameras"]) {
            configs.cameras.push_back(CameraConfig::from_json(cam_json));
        }
    }

    if (j.contains("arms") && j["arms"].is_array()) {
        for (const auto& arm_json : j["arms"]) {
            configs.arms.push_back(ArmConfig::from_json(arm_json));
        }
    }

    if (j.contains("systems") && j["systems"].is_array()) {
        for (const auto& sys_json : j["systems"]) {
            configs.systems.push_back(HardwareSystem::from_json(sys_json));
        }
    }

    if (j.contains("producers") && j["producers"].is_array()) {
        for (const auto& prod_json : j["producers"]) {
            configs.producers.push_back(ProducerConfig::from_json(prod_json));
        }
    }

    if (j.contains("sessions") && j["sessions"].is_array()) {
        for (const auto& session_json : j["sessions"]) {
            configs.sessions.push_back(RecordingSession::from_json(session_json));
        }
    }

    if (j.contains("activities") && j["activities"].is_array()) {
        for (const auto& activity_json : j["activities"]) {
            configs.activities.push_back(ActivityLog::from_json(activity_json));
        }
    }

    return configs;
}

// ConfigManager implementation
ConfigManager::ConfigManager(const std::string& data_file)
    : data_file_(data_file) {
    load_from_file();
}

bool ConfigManager::validate_camera_config(const CameraConfig& config, std::string& error) {
    if (config.type.empty()) {
        error = "Camera type is required";
        return false;
    }

    if (config.name.empty()) {
        error = "Camera name is required";
        return false;
    }

    if (config.type == "opencv") {
        return validate_opencv_camera(config, error);
    } else if (config.type == "realsense") {
        error = "RealSense cameras not yet supported";
        return false;
    } else {
        error = "Unknown camera type: " + config.type;
        return false;
    }
}

bool ConfigManager::validate_opencv_camera(const CameraConfig& config, std::string& error) {
    // Validate using SDK structure
    if (config.device_index < 0) {
        error = "device_index must be >= 0";
        return false;
    }

    if (config.width <= 0 || config.height <= 0) {
        error = "width and height must be positive";
        return false;
    }

    if (config.fps <= 0 || config.fps > 120) {
        error = "fps must be between 1 and 120";
        return false;
    }

    if (config.stream_id.empty()) {
        error = "stream_id is required";
        return false;
    }

    try {
        trossen::hw::camera::OpenCvCameraProducer::Config sdk_cfg;
        sdk_cfg.device_index = config.device_index;
        sdk_cfg.stream_id = config.stream_id;
        sdk_cfg.encoding = config.encoding;
        sdk_cfg.width = config.width;
        sdk_cfg.height = config.height;
        sdk_cfg.fps = config.fps;
        sdk_cfg.use_device_time = config.use_device_time;

        std::cout << "OpenCV camera configuration validated: " << config.name << std::endl;
        return true;
    } catch (const std::exception& e) {
        error = std::string("SDK validation failed: ") + e.what();
        return false;
    }
}

bool ConfigManager::validate_arm_config(const ArmConfig& config, std::string& error) {
    if (config.type.empty()) {
        error = "Arm type is required";
        return false;
    }

    if (config.name.empty()) {
        error = "Arm name is required";
        return false;
    }

    if (config.type == "so101") {
        return validate_so101_arm(config, error);
    } else if (config.type == "widowx") {
        return validate_widowx_arm(config, error);
    } else {
        error = "Unknown arm type: " + config.type;
        return false;
    }
}

bool ConfigManager::validate_so101_arm(const ArmConfig& config, std::string& error) {
    if (config.port.empty()) {
        error = "SO101 arm requires a serial port (e.g., /dev/ttyUSB0)";
        return false;
    }

    if (config.end_effector != "leader" && config.end_effector != "follower") {
        error = "end_effector must be 'leader' or 'follower'";
        return false;
    }

    // Try to create and configure SO101 driver
    try {
        SO101ArmDriver driver;
        SO101EndEffector ee = (config.end_effector == "leader") ?
            SO101EndEffector::leader : SO101EndEffector::follower;

        bool configured = driver.configure(ee, config.port);
        if (!configured) {
            error = "SO101 driver configuration failed for port: " + config.port;
            return false;
        }

        std::cout << "SO101 arm configuration validated: " << config.name
                  << " (" << config.end_effector << " on " << config.port << ")" << std::endl;
        return true;
    } catch (const std::exception& e) {
        error = std::string("SO101 SDK validation failed: ") + e.what();
        return false;
    }
}

bool ConfigManager::validate_widowx_arm(const ArmConfig& config, std::string& error) {
    if (config.serv_ip.empty()) {
        error = "WidowX arm requires an IP address";
        return false;
    }

    // Validate IP address format (basic check)
    std::istringstream iss(config.serv_ip);
    int a, b, c, d;
    char dot;
    if (!(iss >> a >> dot >> b >> dot >> c >> dot >> d) || dot != '.' ||
        a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) {
        error = "Invalid IP address format";
        return false;
    }

    if (config.end_effector != "wxai_v0_leader" && config.end_effector != "wxai_v0_follower") {
        error = "end_effector must be 'wxai_v0_leader' or 'wxai_v0_follower'";
        return false;
    }

    // Note: We cannot fully validate WidowX without attempting connection
    std::cout << "WidowX arm configuration validated: " << config.name
              << " (" << config.end_effector << " at " << config.serv_ip << ")" << std::endl;
    return true;
}

bool ConfigManager::add_camera_config(const CameraConfig& config, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!validate_camera_config(config, error)) {
        return false;
    }

    // Check for duplicate names
    for (const auto& cam : configs_.cameras) {
        if (cam.name == config.name) {
            error = "Camera with name '" + config.name + "' already exists";
            return false;
        }
    }

    configs_.cameras.push_back(config);
    return save_to_file();
}

bool ConfigManager::add_arm_config(const ArmConfig& config, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!validate_arm_config(config, error)) {
        return false;
    }

    // Check for duplicate names
    for (const auto& arm : configs_.arms) {
        if (arm.name == config.name) {
            error = "Arm with name '" + config.name + "' already exists";
            return false;
        }
    }

    configs_.arms.push_back(config);
    return save_to_file();
}

bool ConfigManager::update_camera_config(
    int index,
    const CameraConfig& config,
    std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (index < 0 || index >= static_cast<int>(configs_.cameras.size())) {
        error = "Invalid camera index";
        return false;
    }

    if (!validate_camera_config(config, error)) {
        return false;
    }

    // Check for duplicate names (excluding current index)
    for (size_t i = 0; i < configs_.cameras.size(); i++) {
        if (i != static_cast<size_t>(index) && configs_.cameras[i].name == config.name) {
            error = "Camera with name '" + config.name + "' already exists";
            return false;
        }
    }

    configs_.cameras[index] = config;
    return save_to_file();
}

bool ConfigManager::delete_camera_config(int index, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (index < 0 || index >= static_cast<int>(configs_.cameras.size())) {
        error = "Invalid camera index";
        return false;
    }

    configs_.cameras.erase(configs_.cameras.begin() + index);
    return save_to_file();
}

bool ConfigManager::update_arm_config(int index, const ArmConfig& config, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (index < 0 || index >= static_cast<int>(configs_.arms.size())) {
        error = "Invalid arm index";
        return false;
    }

    if (!validate_arm_config(config, error)) {
        return false;
    }

    // Check for duplicate names (excluding current index)
    for (size_t i = 0; i < configs_.arms.size(); i++) {
        if (i != static_cast<size_t>(index) && configs_.arms[i].name == config.name) {
            error = "Arm with name '" + config.name + "' already exists";
            return false;
        }
    }

    configs_.arms[index] = config;
    return save_to_file();
}

bool ConfigManager::delete_arm_config(int index, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (index < 0 || index >= static_cast<int>(configs_.arms.size())) {
        error = "Invalid arm index";
        return false;
    }

    configs_.arms.erase(configs_.arms.begin() + index);
    return save_to_file();
}

// Hardware system configuration methods
bool ConfigManager::add_system(const HardwareSystem& system, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (system.name.empty()) {
        error = "System name is required";
        return false;
    }

    if (system.producers.empty()) {
        error = "System must contain at least one producer";
        return false;
    }

    // Check for duplicate system IDs
    for (const auto& sys : configs_.systems) {
        if (sys.id == system.id) {
            error = "System ID already exists";
            return false;
        }
    }

    configs_.systems.push_back(system);
    return save_to_file();
}

bool ConfigManager::update_system(
    const std::string& id,
    const HardwareSystem& system,
    std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (system.name.empty()) {
        error = "System name is required";
        return false;
    }

    if (system.producers.empty()) {
        error = "System must contain at least one producer";
        return false;
    }

    // Find system by ID
    auto it = std::find_if(configs_.systems.begin(), configs_.systems.end(),
                          [&id](const HardwareSystem& s) { return s.id == id; });

    if (it == configs_.systems.end()) {
        error = "System not found";
        return false;
    }

    *it = system;
    return save_to_file();
}

bool ConfigManager::delete_system(const std::string& id, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(configs_.systems.begin(), configs_.systems.end(),
                          [&id](const HardwareSystem& s) { return s.id == id; });

    if (it == configs_.systems.end()) {
        error = "System not found";
        return false;
    }

    configs_.systems.erase(it);
    return save_to_file();
}

bool ConfigManager::add_session(
  const RecordingSession& session,
  std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Validate session name
    if (session.name.empty()) {
        error = "Session name is required";
        return false;
    }

    bool has_individual_hardware =
      !session.cameras.empty() || !session.robots.empty();
    bool has_system = !session.system_id.empty();

    if (has_individual_hardware && has_system) {
        error = "Cannot use both individual hardware and system. "
                "Choose one or the other";
        return false;
    }

    if (!has_individual_hardware && !has_system) {
        error = "Must select at least one camera/robot or a hardware system";
        return false;
    }

    // Validate system exists if system_id is provided
    if (has_system) {
        auto sys_it = std::find_if(configs_.systems.begin(),
                                   configs_.systems.end(),
                                   [&](const HardwareSystem& s) {
                                       return s.id == session.system_id;
                                   });
        if (sys_it == configs_.systems.end()) {
            error = "Hardware system not found: " + session.system_id;
            return false;
        }
    }

    // Validate cameras exist
    for (const auto& cam_name : session.cameras) {
        auto cam_it = std::find_if(configs_.cameras.begin(),
                                   configs_.cameras.end(),
                                   [&](const CameraConfig& c) {
                                       return c.name == cam_name;
                                   });
        if (cam_it == configs_.cameras.end()) {
            error = "Camera not found: " + cam_name;
            return false;
        }
    }

    // Validate robots exist
    for (const auto& robot_name : session.robots) {
        auto arm_it = std::find_if(configs_.arms.begin(), configs_.arms.end(),
                                   [&](const ArmConfig& a) {
                                       return a.name == robot_name;
                                   });
        if (arm_it == configs_.arms.end()) {
            error = "Robot not found: " + robot_name;
            return false;
        }
    }

    // Validate episode configuration
    if (session.num_episodes <= 0) {
        error = "Number of episodes must be greater than 0";
        return false;
    }

    if (session.episode_duration <= 0) {
        error = "Episode duration must be greater than 0";
        return false;
    }

    // Validate backend type
    if (session.backend_type != "mcap" && session.backend_type != "lerobot") {
        error = "Backend type must be 'mcap' or 'lerobot'";
        return false;
    }

    // Check for duplicate ID
    auto it = std::find_if(configs_.sessions.begin(), configs_.sessions.end(),
                          [&](const RecordingSession& s) {
                              return s.id == session.id;
                          });
    if (it != configs_.sessions.end()) {
        error = "Session with this ID already exists";
        return false;
    }

    configs_.sessions.push_back(session);
    return save_to_file();
}

bool ConfigManager::update_session(const std::string& id,
                                   const RecordingSession& session,
                                   std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(configs_.sessions.begin(), configs_.sessions.end(),
                          [&id](const RecordingSession& s) {
                              return s.id == id;
                          });

    if (it == configs_.sessions.end()) {
        error = "Session not found";
        return false;
    }

    // Validate session name
    if (session.name.empty()) {
        error = "Session name is required";
        return false;
    }

    // Validate that either individual hardware OR system is selected (not both)
    bool has_individual_hardware =
      !session.cameras.empty() || !session.robots.empty();
    bool has_system = !session.system_id.empty();

    if (has_individual_hardware && has_system) {
        error = "Cannot use both individual hardware and system. "
                "Choose one or the other";
        return false;
    }

    if (!has_individual_hardware && !has_system) {
        error = "Must select at least one camera/robot or a hardware system";
        return false;
    }

    // Validate system exists if system_id is provided
    if (has_system) {
        auto sys_it = std::find_if(configs_.systems.begin(),
                                   configs_.systems.end(),
                                   [&](const HardwareSystem& s) {
                                       return s.id == session.system_id;
                                   });
        if (sys_it == configs_.systems.end()) {
            error = "Hardware system not found: " + session.system_id;
            return false;
        }
    }

    // Validate cameras exist
    for (const auto& cam_name : session.cameras) {
        auto cam_it = std::find_if(configs_.cameras.begin(),
                                   configs_.cameras.end(),
                                   [&](const CameraConfig& c) {
                                       return c.name == cam_name;
                                   });
        if (cam_it == configs_.cameras.end()) {
            error = "Camera not found: " + cam_name;
            return false;
        }
    }

    // Validate robots exist
    for (const auto& robot_name : session.robots) {
        auto arm_it = std::find_if(configs_.arms.begin(), configs_.arms.end(),
                                   [&](const ArmConfig& a) {
                                       return a.name == robot_name;
                                   });
        if (arm_it == configs_.arms.end()) {
            error = "Robot not found: " + robot_name;
            return false;
        }
    }

    // Validate episode configuration
    if (session.num_episodes <= 0) {
        error = "Number of episodes must be greater than 0";
        return false;
    }

    if (session.episode_duration <= 0) {
        error = "Episode duration must be greater than 0";
        return false;
    }

    // Validate backend type
    if (session.backend_type != "mcap" && session.backend_type != "lerobot") {
        error = "Backend type must be 'mcap' or 'lerobot'";
        return false;
    }

    *it = session;
    it->id = id;
    return save_to_file();
}

bool ConfigManager::delete_session(const std::string& id, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(configs_.sessions.begin(), configs_.sessions.end(),
                          [&id](const RecordingSession& s) {
                              return s.id == id;
                          });

    if (it == configs_.sessions.end()) {
        error = "Session not found";
        return false;
    }

    configs_.sessions.erase(it);
    return save_to_file();
}

Configurations ConfigManager::get_configurations() {
    std::lock_guard<std::mutex> lock(mutex_);
    return configs_;
}

bool ConfigManager::save_to_file() {
    try {
        // Read existing file to preserve producers array
        nlohmann::json existing_data;
        std::ifstream read_file(data_file_);
        if (read_file.is_open()) {
            read_file >> existing_data;
            read_file.close();
        }

        std::ofstream file(data_file_);
        if (!file.is_open()) {
            std::cerr << "Failed to open file for writing: " << data_file_ << std::endl;
            return false;
        }

        nlohmann::json j = configs_.to_json();

        // Preserve producers array if it exists
        if (existing_data.contains("producers")) {
            j["producers"] = existing_data["producers"];
        } else {
            j["producers"] = nlohmann::json::array();
        }

        file << j.dump(2);
        file.close();

        std::cout << "Configurations saved to " << data_file_ << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error saving configurations: " << e.what() << std::endl;
        return false;
    }
}

bool ConfigManager::load_from_file() {
    try {
        std::ifstream file(data_file_);
        if (!file.is_open()) {
            // File doesn't exist yet, start with empty configs
            std::cout << "No existing configuration file found, starting fresh" << std::endl;
            configs_ = Configurations();
            return true;
        }

        nlohmann::json j;
        file >> j;
        file.close();

        configs_ = Configurations::from_json(j);
        std::cout << "Loaded " << configs_.cameras.size() << " cameras, "
                  << configs_.arms.size() << " arms, "
                  << configs_.systems.size() << " systems, and "
                  << configs_.sessions.size() << " sessions from "
                  << data_file_ << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error loading configurations: " << e.what() << std::endl;
        configs_ = Configurations();
        return false;
    }
}

bool ConfigManager::save_raw_json(const nlohmann::json& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        std::ofstream file(data_file_);
        if (!file.is_open()) {
            return false;
        }
        file << data.dump(2);
        file.close();

        // Reload configurations to keep in sync
        load_from_file();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error saving raw JSON: " << e.what() << std::endl;
        return false;
    }
}

void ConfigManager::log_activity(const std::string& session_id,
                                  const std::string& session_name,
                                  const std::string& event_type,
                                  const std::string& description) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Get current timestamp as milliseconds since epoch
    auto now = std::chrono::system_clock::now();
    auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    ActivityLog activity;
    activity.timestamp = std::to_string(ms_since_epoch);
    activity.session_id = session_id;
    activity.session_name = session_name;
    activity.event_type = event_type;
    activity.description = description;

    configs_.activities.push_back(activity);

    // Keep only the most recent 500 activities to avoid unbounded growth
    if (configs_.activities.size() > 500) {
        configs_.activities.erase(configs_.activities.begin());
    }

    save_to_file();
}

std::vector<ActivityLog> ConfigManager::get_recent_activities(int limit) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<ActivityLog> recent;
    int start = std::max(0, static_cast<int>(configs_.activities.size()) - limit);

    for (int i = configs_.activities.size() - 1; i >= start; --i) {
        recent.push_back(configs_.activities[i]);
    }

    return recent;
}

void ConfigManager::clear_activities() {
    std::lock_guard<std::mutex> lock(mutex_);
    configs_.activities.clear();
    save_to_file();
}

}  // namespace trossen::config
