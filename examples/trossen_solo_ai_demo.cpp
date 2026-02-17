/**
 * @file trossen_solo_ai_demo.cpp
 * @brief Teleoperation-based data collection demo for Trossen AI robot systems
 *
 * This demo enables data collection for training AI models using a leader-follower
 * teleoperation setup. The leader arm operates in gravity compensation mode, allowing
 * the user to manually guide it. The follower arm mirrors the leader's movements in
 * real-time, while the session manager records:
 *   - Follower arm joint states (30 Hz)
 *   - Camera feeds (configurable FPS)
 *
 * Architecture:
 *   1. Hardware components (arms, cameras) are instantiated from robot config
 *   2. Producers stream data from follower arms and cameras
 *   3. Session manager records data into McapBackend episodes
 *   4. Teleop loop continuously mirrors leader -> follower positions
 *
 * Usage:
 *   ./trossen_solo_ai_demo
 *
 * Configuration:
 *   - Robot config: config/robot_configs.json (trossen_solo_ai section)
 *   - SDK config: config/sdk_config.json
 */

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"
#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/configuration/types/robots/trossen_mobile_ai_config.hpp"
#include "trossen_sdk/configuration/types/robots/trossen_solo_ai_config.hpp"
#include "trossen_sdk/configuration/types/robots/trossen_stationary_ai_config.hpp"
#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"

#include "./demo_utils.hpp"

// ════════════════════════════════════════════════════════════════════════════════
// HELPER FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════════

/**
 * @brief Set an arm to teleop mode (gravity compensation)
 *
 * Enables external_effort mode with zero effort, allowing manual manipulation
 * of the arm while maintaining position feedback.
 *
 * @param driver The arm driver to configure
 */
void enable_teleop_mode(std::shared_ptr<trossen_arm::TrossenArmDriver> driver) {
  driver->set_all_modes(trossen_arm::Mode::external_effort);
  driver->set_all_external_efforts(
    std::vector<double>(driver->get_num_joints(), 0.0),
    0.0,
    false);
}

/**
 * @brief Move arm to target positions and wait for completion
 *
 * Switches arm to position mode, commands target positions, and blocks until
 * the movement is complete.
 *
 * @param driver The arm driver to move
 * @param positions Target joint positions
 * @param moving_time_s Duration for the movement in seconds
 */
void move_arm_to_positions(
    std::shared_ptr<trossen_arm::TrossenArmDriver> driver,
    const std::vector<double>& positions,
    float moving_time_s) {
  driver->set_all_modes(trossen_arm::Mode::position);
  driver->set_all_positions(positions, moving_time_s, false);
  std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
}

/**
 * @brief Synchronize follower arm to leader's current position
 *
 * Temporarily locks leader in position mode, moves follower to match,
 * then re-enables leader teleop mode.
 *
 * @param leader_driver Leader arm driver
 * @param follower_driver Follower arm driver
 * @param moving_time_s Duration for synchronization movement
 */
void sync_follower_to_leader(
    std::shared_ptr<trossen_arm::TrossenArmDriver> leader_driver,
    std::shared_ptr<trossen_arm::TrossenArmDriver> follower_driver,
    float moving_time_s) {
  // Temporarily lock leader arm
  leader_driver->set_all_modes(trossen_arm::Mode::position);

  // Get current leader position and command follower to match
  auto leader_positions = leader_driver->get_all_positions();
  follower_driver->set_all_positions(leader_positions, moving_time_s, false);
  std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));

  // Re-enable leader teleop mode
  enable_teleop_mode(leader_driver);
}

// ════════════════════════════════════════════════════════════════════════════════
// HARDWARE COMPONENT CREATION
// ════════════════════════════════════════════════════════════════════════════════

/**
 * @brief Container for all hardware components created from robot config
 */
struct HardwareComponents {
  /// Hardware component wrappers (used for producers)
  std::vector<std::shared_ptr<trossen::hw::HardwareComponent>> arms;
  std::vector<std::shared_ptr<trossen::hw::HardwareComponent>> cameras;
  std::shared_ptr<trossen::hw::HardwareComponent> mobile_base;

  /// Raw arm drivers for direct teleoperation control
  std::vector<std::shared_ptr<trossen_arm::TrossenArmDriver>> arm_drivers;
};

/**
 * @brief Instantiate hardware components from robot configuration
 *
 * Creates HardwareComponent wrappers for all devices specified in the robot
 * config (arms and cameras). For arms, also extracts raw driver pointers for
 * direct teleoperation control.
 *
 * @tparam RobotConfig Robot configuration type (TrossenAiSoloConfig, etc.)
 * @param robot_config Loaded robot configuration object
 * @return HardwareComponents struct containing all instantiated hardware
 */
template<typename RobotConfig>
HardwareComponents create_hardware_components(const RobotConfig& robot_config) {
  HardwareComponents hw;

  // ──────────────────────────────────────────────────────────
  // Create arm hardware components
  // ──────────────────────────────────────────────────────────

  for (const auto& arm_cfg : robot_config.arms) {
    nlohmann::json arm_hw_cfg = {
      {"ip_address", arm_cfg.ip_address},
      {"model", arm_cfg.model},
      {"end_effector", arm_cfg.end_effector}
    };
    auto arm = trossen::hw::HardwareRegistry::create(
      "trossen_arm", arm_cfg.id, arm_hw_cfg);
    hw.arms.push_back(arm);

    // Extract the raw driver for teleoperation
    auto arm_component = std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(arm);
    if (arm_component) {
      hw.arm_drivers.push_back(arm_component->get_hardware());
    }
  }

  // ──────────────────────────────────────────────────────────
  // Create camera hardware components
  // ──────────────────────────────────────────────────────────

  for (const auto& cam_cfg : robot_config.cameras) {
    nlohmann::json hw_cfg = {
      {"width", cam_cfg.width},
      {"height", cam_cfg.height},
      {"fps", cam_cfg.fps}
    };

    if (cam_cfg.type == "opencv_camera") {
      int device_index = std::stoi(cam_cfg.unique_device_id.substr(10));
      hw_cfg["device_index"] = device_index;
      hw_cfg["backend"] = "v4l2";
    } else if (cam_cfg.type == "realsense_camera") {
      hw_cfg["serial_number"] = cam_cfg.unique_device_id;
      hw_cfg["use_depth"] = cam_cfg.enable_depth;
      hw_cfg["force_hardware_reset"] = false;
    }

    auto hw_component = trossen::hw::HardwareRegistry::create(
      cam_cfg.type, cam_cfg.id, hw_cfg);
    hw.cameras.push_back(hw_component);
  }

  return hw;
}

// ════════════════════════════════════════════════════════════════════════════════
// PRODUCER REGISTRATION
// ════════════════════════════════════════════════════════════════════════════════

/**
 * @brief Create and register data producers with the session manager
 *
 * Creates producers for:
 *   - Follower arm joint states (30 Hz) - Only follower arms are recorded!
 *   - Camera image feeds (config-specified FPS)
 *
 * Note: Leader arms are NOT recorded since they are manually controlled.
 * Only the follower arm's mimicked positions are recorded for training.
 *
 * @tparam RobotConfig Robot configuration type
 * @param mgr Session manager to register producers with
 * @param robot_config Robot configuration containing stream settings
 * @param hw Hardware components to create producers from
 */
template<typename RobotConfig>
void create_and_register_producers(
    trossen::runtime::SessionManager& mgr,
    const RobotConfig& robot_config,
    const HardwareComponents& hw) {

  std::cout << "Creating producers...\n";

  // ──────────────────────────────────────────────────────────
  // Create arm producers (only for follower arms)
  // ──────────────────────────────────────────────────────────

  size_t follower_count = 0;
  for (size_t i = 0; i < robot_config.arms.size(); ++i) {
    const auto& arm_cfg = robot_config.arms[i];

    // Only create producers for follower arms
    if (arm_cfg.end_effector.find("follower") != std::string::npos) {
      const float arm_rate_hz = 30.0f;  // Fixed at 30Hz for recording
      auto joint_period = std::chrono::milliseconds(
        static_cast<int>(1000.0f / arm_rate_hz));

      nlohmann::json prod_cfg = {
        {"stream_id", arm_cfg.id + "/joint_states"},
        {"use_device_time", false}
      };

      auto producer = trossen::runtime::ProducerRegistry::create(
        "trossen_arm", hw.arms[i], prod_cfg);
      mgr.add_producer(producer, joint_period);
      std::cout << "  ✓ Follower arm producer " << arm_cfg.id
                << " (" << arm_rate_hz << " Hz)\n";
      follower_count++;
    }
  }

  // ──────────────────────────────────────────────────────────
  // Create camera producers
  // ──────────────────────────────────────────────────────────

  for (size_t i = 0; i < hw.cameras.size(); ++i) {
    const auto& cam_cfg = robot_config.cameras[i];
    auto camera_period = std::chrono::milliseconds(
      static_cast<int>(1000.0f / cam_cfg.fps));

    nlohmann::json prod_cfg = {
      {"stream_id", cam_cfg.id + "/image"},
      {"encoding", cam_cfg.encoding},
      {"use_device_time", false},
      {"width", cam_cfg.width},
      {"height", cam_cfg.height},
      {"fps", cam_cfg.fps}
    };

    auto cam_producer = trossen::runtime::ProducerRegistry::create(
      cam_cfg.type, hw.cameras[i], prod_cfg);

    mgr.add_producer(cam_producer, camera_period);
    std::cout << "  ✓ Camera producer " << cam_cfg.id << " (" << cam_cfg.fps
              << " Hz, " << cam_cfg.width << "x" << cam_cfg.height << ")\n";
  }

  std::cout << "\nProducers registered. Ready to record.\n";
  std::cout << "  Recording: " << follower_count << " follower arm(s) + "
            << hw.cameras.size() << " cameras = "
            << (follower_count + hw.cameras.size()) << " data streams\n\n";
}

// ════════════════════════════════════════════════════════════════════════════════
// MAIN ENTRY POINT
// ════════════════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
  try {
    std::cout << "\n════════════════════════════════════════════════════════════\n";
    std::cout << "  Trossen AI Solo Demo\n";
    std::cout << "════════════════════════════════════════════════════════════\n\n";

    // ══════════════════════════════════════════════════════════════════════════════
    // INITIALIZATION
    // ══════════════════════════════════════════════════════════════════════════════

    // ─────────── Load SDK Configuration ───────────

    const std::string sdk_config_file = "config/sdk_config.json";
    if (!std::filesystem::exists(sdk_config_file)) {
      std::cerr << "Error: " << sdk_config_file << " not found!\n";
      return 1;
    }

    auto sdk_json = trossen::configuration::JsonLoader::load(sdk_config_file);
    trossen::configuration::GlobalConfig::instance().load_from_json(sdk_json);

    // ─────────── Load Robot Configuration ───────────

    const std::string robot_config_file = "config/robot_configs.json";
    if (!std::filesystem::exists(robot_config_file)) {
      std::cerr << "Error: " << robot_config_file << " not found!\n";
      return 1;
    }

    auto config_json = trossen::configuration::JsonLoader::load(robot_config_file);

    // Parse Trossen AI Solo configuration
    auto robot_config = trossen::configuration::TrossenAiSoloConfig::from_json(
      config_json["trossen_solo_ai"]);

    // ─────────── Create Hardware Components ───────────

    auto hw = create_hardware_components(robot_config);

    // ─────────── Initialize Session Manager ───────────

    trossen::runtime::SessionManager mgr;

    std::cout << "\nInitialized Session Manager\n";
    std::cout << "  Starting episode index: " << mgr.stats().current_episode_index << "\n";
    if (mgr.stats().current_episode_index > 0) {
      std::cout << "  (Resuming from existing episodes in directory)\n";
    }
    std::cout << "\n";

    // ─────────── Create and Register Producers ───────────
    create_and_register_producers(mgr, robot_config, hw);

    // Install signal handler for graceful shutdown (Ctrl+C)
    trossen::demo::install_signal_handler();

    // ══════════════════════════════════════════════════════════════════════════════
    // ARM SETUP AND TELEOP MODE
    // ══════════════════════════════════════════════════════════════════════════════

    const float moving_time_s = 2.0f;  // Duration for all arm movements

    // ─────────── Identify Leader and Follower Arms ───────────
    // Leader: manually controlled (gravity compensation mode)
    // Follower: mirrors leader (position mode), recorded for training
    std::shared_ptr<trossen_arm::TrossenArmDriver> leader_driver;
    std::shared_ptr<trossen_arm::TrossenArmDriver> follower_driver;

    for (size_t i = 0; i < robot_config.arms.size(); ++i) {
      const auto& arm_cfg = robot_config.arms[i];
      if (arm_cfg.end_effector.find("leader") != std::string::npos) {
        leader_driver = hw.arm_drivers[i];
      } else if (arm_cfg.end_effector.find("follower") != std::string::npos) {
        follower_driver = hw.arm_drivers[i];
      }
    }

    if (!leader_driver || !follower_driver) {
      std::cerr << "Error: Could not find leader and follower arms\n";
      return 1;
    }

    // ─────────── Configure Follower Arm ───────────
    // Increase gripper joint tolerance to prevent overshoot errors
    auto joint_limits = follower_driver->get_joint_limits();
    joint_limits[follower_driver->get_num_joints() - 1].position_tolerance = 0.01;
    follower_driver->set_joint_limits(joint_limits);

    // Move both arms to staged positions
    std::cout << "\nStaging arms...\n";
    move_arm_to_positions(leader_driver, trossen::demo::STAGED_POSITIONS, moving_time_s);
    move_arm_to_positions(follower_driver, trossen::demo::STAGED_POSITIONS, moving_time_s);
    std::cout << "  ✓ Arms staged to starting positions\n";

    // Enable teleop mode on leader arm
    std::cout << "\n!! Enabling teleop mode !!\n";
    std::cout << "   Leader: gravity compensation (external_effort mode)\n";
    std::cout << "   Follower: will mirror leader positions\n\n";
    enable_teleop_mode(leader_driver);

    // Allow time for mode transition to stabilize
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // ══════════════════════════════════════════════════════════════════════════════
    // RECORDING LOOP
    // ══════════════════════════════════════════════════════════════════════════════
    // Record multiple episodes until user requests stop (Ctrl+C).
    // Each iteration:
    //   1. Sync follower to leader position
    //   2. Start episode recording
    //   3. Run teleop loop (leader -> follower mirroring)
    //   4. Stop episode and save data
    //   5. Display episode statistics

    const std::string root = trossen::io::backends::get_default_root_path().string();

    while (true) {
      if (trossen::demo::g_stop_requested) {
        std::cout << "\n\nStopping at user request (Ctrl+C).\n";
        break;
      }

      // Display episode header
      mgr.print_episode_header();

      // Synchronize follower to leader's current position before recording
      sync_follower_to_leader(leader_driver, follower_driver, moving_time_s);

      // ─────────── Start Episode Recording ───────────
      if (!mgr.start_episode()) {
        std::cerr << "✗ Failed to start episode " << mgr.stats().current_episode_index << "\n";
        break;
      }

      // Capture episode metadata for post-recording statistics
      uint32_t recording_episode_index = mgr.stats().current_episode_index;
      auto episode_start_time = std::chrono::steady_clock::now();

      std::cout << "Recording...\n";

      // Start background thread to monitor and display recording statistics
      mgr.start_async_monitoring();

      // ─────────── Teleoperation Loop (Separate Thread) ───────────
      // Continuously mirror leader positions to follower while episode is active
      std::thread teleop_thread([&]() {
        while (mgr.is_episode_active() && !trossen::demo::g_stop_requested) {
          auto leader_js = leader_driver->get_all_positions();
          follower_driver->set_all_positions(leader_js, 0.0f, false);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      });

      // ─────────── Wait for Episode Completion ───────────
      // Main thread blocks until auto-stop conditions are met
      mgr.wait_for_auto_stop();

      // Handle early termination from Ctrl+C
      if (trossen::demo::g_stop_requested && mgr.is_episode_active()) {
        mgr.stop_episode();
      }

      // Retrieve final statistics (joins background monitoring thread)
      trossen::runtime::SessionManager::Stats last_stats = mgr.get_async_monitor_stats();

      // Clean up teleoperation thread
      if (teleop_thread.joinable()) {
        teleop_thread.join();
      }

      // ─────────── Episode Statistics ───────────
      // Calculate actual wall-clock time (vs configured recording duration)
      auto episode_end_time = std::chrono::steady_clock::now();
      double actual_duration =
        std::chrono::duration<double>(episode_end_time - episode_start_time).count();

      // Display episode summary with file location
      std::string file_path = trossen::demo::generate_episode_path(
        root,
        recording_episode_index);
      trossen::demo::print_episode_summary(file_path, last_stats);

      // Sanity check: verify expected record counts
      trossen::demo::SanityCheckConfig sanity_cfg{
        last_stats.recording_duration_s.value_or(0.0),
        1,  // 1 joint producer (follower)
        30.0f,  // arm rate
        static_cast<int>(hw.cameras.size()),
        robot_config.cameras[0].fps,
        5.0  // 5% tolerance
      };
      trossen::demo::perform_sanity_check(
        recording_episode_index,
        last_stats.records_written_current,
        sanity_cfg);

      // Check if user requested stop
      if (trossen::demo::g_stop_requested) {
        std::cout << "\nStopping at user request (Ctrl+C).\n";
        break;
      }
    }

    // ══════════════════════════════════════════════════════════════════════════════
    // SHUTDOWN AND CLEANUP
    // ══════════════════════════════════════════════════════════════════════════════

    mgr.shutdown();

    // Return arms to safe positions: staged -> sleep (all zeros)
    std::cout << "\nReturning arms to starting positions...\n";

    // First move to staged positions
    move_arm_to_positions(leader_driver, trossen::demo::STAGED_POSITIONS, moving_time_s);
    move_arm_to_positions(follower_driver, trossen::demo::STAGED_POSITIONS, moving_time_s);

    // Then move to sleep position (all joints at 0.0)
    std::vector<double> sleep_positions(leader_driver->get_num_joints(), 0.0);
    move_arm_to_positions(leader_driver, sleep_positions, moving_time_s);
    move_arm_to_positions(follower_driver, sleep_positions, moving_time_s);

    std::cout << "Arms returned to sleep position.\n";

    auto final_stats = mgr.stats();
    trossen::demo::print_final_summary(final_stats.total_episodes_completed, root);

    std::cout << "\nDemo complete.\n";
  } catch (const std::exception& e) {
    std::cerr << "\nError: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
