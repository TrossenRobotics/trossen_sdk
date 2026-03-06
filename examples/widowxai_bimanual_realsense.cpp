/**
 * @file widowxai_bimanual_realsense.cpp
 * @brief Complete Bimanual AI Kit demo with all 4 arms recorded and RealSense cameras
 *
 * This demo records all 4 arms (both leaders and followers) along with RealSense cameras:
 * - 4 Trossen AI arms (all recorded: 2 leaders + 2 followers)
 * - Session Manager for multi-episode recording
 * - MCAP backend for data storage
 * - Multiple RealSense camera producers for RGB capture
 * - Teleop control via leaders with follower mirroring
 *
 * Hardware Configuration:
 *   Leader Left:    192.168.1.3 (teleop control, recorded)
 *   Leader Right:   192.168.1.2 (teleop control, recorded)
 *   Follower Left:  192.168.1.5 (mirrors leader left, recorded)
 *   Follower Right: 192.168.1.4 (mirrors leader right, recorded)
 *   Cameras:        Multiple RealSense cameras (RGB only)
 *
 * Usage:
 *   ./widowxai_bimanual_realsense
 */

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"
#include "nlohmann/json.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/arm/arm_producer.hpp"
#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"

#include "./demo_utils.hpp"

struct Config {
  std::string root = trossen::io::backends::get_default_root_path().string();

  // Camera settings
  std::string camera_serial = "128422271347";  // RealSense camera serial number
  int camera_width = 1280;
  int camera_height = 720;
  int camera_fps = 30;

  // Arm settings
  float joint_rate_hz = 200.0f;
  std::string leader_left_ip = "192.168.1.3";
  std::string leader_right_ip = "192.168.1.2";
  std::string follower_left_ip = "192.168.1.5";
  std::string follower_right_ip = "192.168.1.4";
};



int main(int argc, char** argv) {
  if (!std::filesystem::exists("config/sdk_config.json")) {
    std::cerr << "Error: config/sdk_config.json not found!" << std::endl;
    return 1;
  }

  // Create and load global configuration
  auto j = trossen::configuration::JsonLoader::load("config/sdk_config.json");
  trossen::configuration::GlobalConfig::instance().load_from_json(j);

  Config cfg;

  // Print configuration
  std::vector<std::string> config_lines = {
    "Root directory:       " + cfg.root,
    "Backend:              MCAP",
    "Leader Left IP:       " + cfg.leader_left_ip + " (teleop, recorded)",
    "Leader Right IP:      " + cfg.leader_right_ip + " (teleop, recorded)",
    "Follower Left IP:     " + cfg.follower_left_ip + " (recorded)",
    "Follower Right IP:    " + cfg.follower_right_ip + " (recorded)",
    "Joint rate:           " + std::to_string(cfg.joint_rate_hz) + " Hz (all 4 arms)",
    "Camera Serial:        " + cfg.camera_serial,
    "Camera Resolution:    " + std::to_string(cfg.camera_width) + "x" +
    std::to_string(cfg.camera_height) + " @ " + std::to_string(cfg.camera_fps) + " fps (RGB)"
  };

  trossen::demo::print_config_banner("Bimanual AI Kit with RealSense Demo", config_lines);

  // Install signal handler for graceful shutdown
  trossen::demo::install_signal_handler();

  // Create root directory
  std::filesystem::create_directories(cfg.root);

  // ──────────────────────────────────────────────────────────
  // Initialize hardware (if not using mock)
  // ──────────────────────────────────────────────────────────

  std::shared_ptr<trossen_arm::TrossenArmDriver> leader_left_driver;
  std::shared_ptr<trossen_arm::TrossenArmDriver> leader_right_driver;
  std::shared_ptr<trossen_arm::TrossenArmDriver> follower_left_driver;
  std::shared_ptr<trossen_arm::TrossenArmDriver> follower_right_driver;

  std::shared_ptr<trossen::hw::HardwareComponent> leader_left_component;
  std::shared_ptr<trossen::hw::HardwareComponent> leader_right_component;
  std::shared_ptr<trossen::hw::HardwareComponent> follower_left_component;
  std::shared_ptr<trossen::hw::HardwareComponent> follower_right_component;

  const float moving_time_s = 2.0f;

  std::cout << "Initializing hardware...\n";

    // Create leader left via registry
    nlohmann::json leader_left_hw_cfg = {
      {"ip_address", cfg.leader_left_ip},
      {"model", "wxai_v0"},
      {"end_effector", "wxai_v0_leader"}
    };
    leader_left_component = trossen::hw::HardwareRegistry::create(
      "trossen_arm", "leader_left", leader_left_hw_cfg, true);
    auto leader_left_comp_typed = std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(
      leader_left_component);
    leader_left_driver = leader_left_comp_typed->get_hardware();
    std::cout << "  ✓ Leader Left configured (" << cfg.leader_left_ip << ")\n";

    // Create leader right via registry
    nlohmann::json leader_right_hw_cfg = {
      {"ip_address", cfg.leader_right_ip},
      {"model", "wxai_v0"},
      {"end_effector", "wxai_v0_leader"}
    };
    leader_right_component = trossen::hw::HardwareRegistry::create(
      "trossen_arm", "leader_right", leader_right_hw_cfg, true);
    auto leader_right_comp_typed =
      std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(leader_right_component);
    leader_right_driver = leader_right_comp_typed->get_hardware();
    std::cout << "  ✓ Leader Right configured (" << cfg.leader_right_ip << ")\n";

    // Create follower left via registry
    nlohmann::json follower_left_hw_cfg = {
      {"ip_address", cfg.follower_left_ip},
      {"model", "wxai_v0"},
      {"end_effector", "wxai_v0_follower"}
    };
    follower_left_component = trossen::hw::HardwareRegistry::create(
      "trossen_arm", "follower_left", follower_left_hw_cfg, true);
    auto follower_left_comp_typed =
      std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(
        follower_left_component);
    follower_left_driver = follower_left_comp_typed->get_hardware();
    std::cout << "  ✓ Follower Left configured (" << cfg.follower_left_ip << ")\n";

    // Create follower right via registry
    nlohmann::json follower_right_hw_cfg = {
      {"ip_address", cfg.follower_right_ip},
      {"model", "wxai_v0"},
      {"end_effector", "wxai_v0_follower"}
    };
    follower_right_component = trossen::hw::HardwareRegistry::create(
      "trossen_arm", "follower_right", follower_right_hw_cfg, true);
    auto follower_right_comp_typed =
      std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(follower_right_component);
    follower_right_driver = follower_right_comp_typed->get_hardware();
    std::cout << "  ✓ Follower Right configured (" << cfg.follower_right_ip << ")\n";

    // Stage all arms to starting positions
    leader_left_driver->set_all_modes(trossen_arm::Mode::position);
    leader_right_driver->set_all_modes(trossen_arm::Mode::position);
    follower_left_driver->set_all_modes(trossen_arm::Mode::position);
    follower_right_driver->set_all_modes(trossen_arm::Mode::position);

    leader_left_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);
    leader_right_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);
    follower_left_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);
    follower_right_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);

    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
    std::cout << "  ✓ All arms staged to starting positions\n";

    // Adjust follower joint limits for gripper tolerance
    auto left_limits = follower_left_driver->get_joint_limits();
    left_limits[follower_left_driver->get_num_joints() - 1].position_tolerance = 0.01;
    follower_left_driver->set_joint_limits(left_limits);

  auto right_limits = follower_right_driver->get_joint_limits();
  right_limits[follower_right_driver->get_num_joints() - 1].position_tolerance = 0.01;
  follower_right_driver->set_joint_limits(right_limits);

  trossen::runtime::SessionManager mgr;

  std::cout << "\nInitialized Session Manager\n";
  std::cout << "  Starting episode index: " << mgr.stats().current_episode_index << "\n";
  if (mgr.stats().current_episode_index > 0) {
    std::cout << "  (Resuming from existing episodes in directory)\n";
  }
  std::cout << "\n";

  // ──────────────────────────────────────────────────────────
  // Create and register producers
  // ──────────────────────────────────────────────────────────

  std::cout << "Creating producers...\n";

  auto joint_period = std::chrono::milliseconds(static_cast<int>(1000.0f / cfg.joint_rate_hz));

  // Joint state producers for ALL 4 arms (leaders + followers)
  std::vector<std::shared_ptr<trossen::hw::PolledProducer>> joint_producers;

  // Create producer for leader left via registry
    nlohmann::json leader_left_prod_cfg = {
      {"stream_id", "leader_left"},
      {"use_device_time", false}
    };
    auto leader_left_prod = trossen::runtime::ProducerRegistry::create(
      "trossen_arm", leader_left_component, leader_left_prod_cfg);
    joint_producers.push_back(leader_left_prod);
    mgr.add_producer(leader_left_prod, joint_period);
    std::cout << "  ✓ Leader Left producer (" << cfg.joint_rate_hz << " Hz)\n";

    // Create producer for leader right via registry
    nlohmann::json leader_right_prod_cfg = {
      {"stream_id", "leader_right"},
      {"use_device_time", false}
    };
    auto leader_right_prod = trossen::runtime::ProducerRegistry::create(
      "trossen_arm", leader_right_component, leader_right_prod_cfg);
    joint_producers.push_back(leader_right_prod);
    mgr.add_producer(leader_right_prod, joint_period);
    std::cout << "  ✓ Leader Right producer (" << cfg.joint_rate_hz << " Hz)\n";

    // Create producer for follower left via registry
    nlohmann::json follower_left_prod_cfg = {
      {"stream_id", "follower_left"},
      {"use_device_time", false}
    };
    auto follower_left_prod = trossen::runtime::ProducerRegistry::create(
      "trossen_arm", follower_left_component, follower_left_prod_cfg);
    joint_producers.push_back(follower_left_prod);
    mgr.add_producer(follower_left_prod, joint_period);
    std::cout << "  ✓ Follower Left producer (" << cfg.joint_rate_hz << " Hz)\n";

    // Create producer for follower right via registry
    nlohmann::json follower_right_prod_cfg = {
      {"stream_id", "follower_right"},
      {"use_device_time", false}
    };
    auto follower_right_prod = trossen::runtime::ProducerRegistry::create(
      "trossen_arm", follower_right_component, follower_right_prod_cfg);
    joint_producers.push_back(follower_right_prod);
  mgr.add_producer(follower_right_prod, joint_period);
  std::cout << "  ✓ Follower Right producer (" << cfg.joint_rate_hz << " Hz)\n";

  // ──────────────────────────────────────────────────────────
  // RealSense Camera producer (RGB only)
  // ──────────────────────────────────────────────────────────
  auto camera_period = std::chrono::milliseconds(static_cast<int>(1000.0f / cfg.camera_fps));

  // Create hardware component for RealSense camera
  nlohmann::json camera_hw_cfg = {
    {"serial_number", cfg.camera_serial},
    {"width", cfg.camera_width},
    {"height", cfg.camera_height},
    {"fps", cfg.camera_fps},
    {"use_depth", false},
    {"force_hardware_reset", false}
  };

  auto camera_component = trossen::hw::HardwareRegistry::create(
    "realsense_camera", "camera_0", camera_hw_cfg);
  std::cout << "  ✓ RealSense camera hardware initialized (" << cfg.camera_serial << ")\n";

  // Create RGB producer
  nlohmann::json rgb_prod_cfg = {
    {"stream_id", "camera_0"},
    {"encoding", "bgr8"},
    {"use_device_time", true},
    {"width", cfg.camera_width},
    {"height", cfg.camera_height},
    {"fps", cfg.camera_fps}
  };

  auto rgb_prod = trossen::runtime::ProducerRegistry::create(
    "realsense_camera", camera_component, rgb_prod_cfg);
  mgr.add_producer(rgb_prod, camera_period);
  std::cout << "  ✓ RealSense camera producer (" << cfg.camera_fps << " Hz, "
            << cfg.camera_width << "x" << cfg.camera_height << " RGB)\n";

  std::cout << "\nProducers registered. Ready to record.\n";
  std::cout << "  Recording: 4 arms (all leaders + followers) + 1 RealSense camera\n";

  while (true) {
    if (trossen::demo::g_stop_requested) {
      std::cout << "\n\nStopping at user request (Ctrl+C).\n";
      break;
    }

    // Lock the leader arms, move followers to match
    if (leader_left_driver && leader_right_driver &&
        follower_left_driver && follower_right_driver) {
      std::cout << "\nLocking leader arms and moving followers to match...\n";

      // Lock leaders in position mode
      leader_left_driver->set_all_modes(trossen_arm::Mode::position);
      leader_right_driver->set_all_modes(trossen_arm::Mode::position);

      // Get current leader positions
      auto leader_left_positions = leader_left_driver->get_all_positions();
      auto leader_right_positions = leader_right_driver->get_all_positions();

      // Move followers to match
      follower_left_driver->set_all_modes(trossen_arm::Mode::position);
      follower_right_driver->set_all_modes(trossen_arm::Mode::position);
      follower_left_driver->set_all_positions(leader_left_positions, moving_time_s, false);
      follower_right_driver->set_all_positions(leader_right_positions, moving_time_s, false);
      std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
      std::cout << "  ✓ Followers moved to match leaders\n";

      // Re-enable teleop on leaders
      std::cout << "\n!! Enabling teleop mode !!\n";
      std::cout << "   Leader Left & Right: gravity compensation (external_effort mode)\n";
      std::cout << "   Follower Left: will mirror Leader Left\n";
      std::cout << "   Follower Right: will mirror Leader Right\n";
      std::cout << "   All 4 arms will be recorded\n\n";
      leader_left_driver->set_all_modes(trossen_arm::Mode::external_effort);
      leader_left_driver->set_all_external_efforts(
        std::vector<double>(leader_left_driver->get_num_joints(), 0.0),
        0.0,
        false);
      leader_right_driver->set_all_modes(trossen_arm::Mode::external_effort);
      leader_right_driver->set_all_external_efforts(
        std::vector<double>(leader_right_driver->get_num_joints(), 0.0),
        0.0,
        false);
    }

    // Display episode header
    mgr.print_episode_header();

    // Start episode
    if (!mgr.start_episode()) {
      std::cerr << "✗ Failed to start episode " << mgr.stats().current_episode_index << "\n";
      break;
    }

    // Capture the episode index that's currently recording
    uint32_t recording_episode_index = mgr.stats().current_episode_index;

    // Track episode start time for actual duration calculation
    auto episode_start_time = std::chrono::steady_clock::now();

    std::cout << "Recording...\n";

    // Teleop loop - mirror leader positions to followers
    std::thread teleop_thread([&]() {
        while (mgr.is_episode_active() && !trossen::demo::g_stop_requested) {
          // Mirror left side
          auto leader_left_js = leader_left_driver->get_all_positions();
          follower_left_driver->set_all_positions(leader_left_js, 0.0f, false);

          // Mirror right side
          auto leader_right_js = leader_right_driver->get_all_positions();
          follower_right_driver->set_all_positions(leader_right_js, 0.0f, false);

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });

    // Blocking monitor call: keeps the main thread alive while the episode is recording,
    // prevents threads from joining before data is collected, and updates/prints status logs.
    trossen::runtime::SessionManager::Stats last_stats = mgr.monitor_episode();

    // Stop episode and wait for teleop thread
    if (mgr.is_episode_active()) {
      mgr.stop_episode();
    }

    if (teleop_thread.joinable()) {
      teleop_thread.join();
    }

    // Calculate actual episode duration
    auto episode_end_time = std::chrono::steady_clock::now();
    double actual_duration =
      std::chrono::duration<double>(episode_end_time - episode_start_time).count();

    // Build the file path and print summary
    std::string file_path = trossen::demo::generate_episode_path(
      cfg.root,
      recording_episode_index);
    trossen::demo::print_episode_summary(file_path, last_stats);

    trossen::demo::SanityCheckConfig sanity_cfg{
      last_stats.elapsed.count(),
      4,  // 4 joint producers (all arms)
      cfg.joint_rate_hz,
      1,  // 1 RealSense camera (RGB only)
      cfg.camera_fps,
      5.0  // 5% tolerance
    };
    perform_sanity_check(recording_episode_index, last_stats.records_written_current, sanity_cfg);

    // Check if user requested stop
    if (trossen::demo::g_stop_requested) {
      std::cout << "\nStopping at user request (Ctrl+C).\n";
      break;
    }
  }

  mgr.shutdown();

  // Return arms to starting position
  if (leader_left_driver && leader_right_driver &&
      follower_left_driver && follower_right_driver) {
    std::cout << "\nReturning arms to starting positions...\n";

    leader_left_driver->set_all_modes(trossen_arm::Mode::position);
    leader_right_driver->set_all_modes(trossen_arm::Mode::position);
    follower_left_driver->set_all_modes(trossen_arm::Mode::position);
    follower_right_driver->set_all_modes(trossen_arm::Mode::position);

    leader_left_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);
    leader_right_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);
    follower_left_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);
    follower_right_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);

    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));

    // Move to sleep positions
    std::cout << "Moving arms to sleep positions...\n";
    leader_left_driver->set_all_positions(
      std::vector<double>(leader_left_driver->get_num_joints(), 0.0),
      moving_time_s,
      false);
    leader_right_driver->set_all_positions(
      std::vector<double>(leader_right_driver->get_num_joints(), 0.0),
      moving_time_s,
      false);
    follower_left_driver->set_all_positions(
      std::vector<double>(follower_left_driver->get_num_joints(), 0.0),
      moving_time_s,
      false);
    follower_right_driver->set_all_positions(
      std::vector<double>(follower_right_driver->get_num_joints(), 0.0),
      moving_time_s,
      false);
    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
  }

  auto final_stats = mgr.stats();
  std::vector<std::string> extra_info = {
    "Data streams:         4 arms (all recorded) + 1 RealSense camera (RGB)"
  };
  trossen::demo::print_final_summary(
    final_stats.total_episodes_completed,
    cfg.root,
    extra_info);

  return 0;
}
