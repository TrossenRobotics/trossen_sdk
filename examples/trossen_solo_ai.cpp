/**
 * @file trossen_solo_ai.cpp
 * @brief Single arm AI Kit demo with leader, follower, and 2 RealSense cameras
 *
 * This demo records a single arm pair along with 2 RealSense cameras:
 * - 1 Trossen AI leader arm (teleop control, recorded)
 * - 1 Trossen AI follower arm (mirrors leader, recorded)
 * - 2 RealSense camera producers for RGB capture
 * - Session Manager for multi-episode recording
 * - MCAP backend for data storage
 *
 * Hardware Configuration:
 *   Leader:     192.168.1.3 (teleop control, recorded)
 *   Follower:   192.168.1.5 (mirrors leader, recorded)
 *   Cameras:    2 RealSense cameras (RGB only)
 *
 * Usage:
 *   ./trossen_solo_ai
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
  std::vector<std::string> camera_serials = {
    "128422271347",  // Camera 0 - TODO: Update with your serial number
    "218622270304"   // Camera 1 - TODO: Update with your serial number
  };
  int camera_width = 640;
  int camera_height = 480;
  int camera_fps = 30;

  // Arm settings
  float joint_rate_hz = 30.0f;
  std::string leader_ip = "192.168.1.3";
  std::string follower_ip = "192.168.1.5";
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
    "Leader IP:            " + cfg.leader_ip + " (teleop, recorded)",
    "Follower IP:          " + cfg.follower_ip + " (recorded)",
    "Joint rate:           " + std::to_string(cfg.joint_rate_hz) + " Hz",
    "Camera 0 Serial:      " + cfg.camera_serials[0],
    "Camera 1 Serial:      " + cfg.camera_serials[1],
    "Camera Resolution:    " + std::to_string(cfg.camera_width) + "x" +
    std::to_string(cfg.camera_height) + " @ " + std::to_string(cfg.camera_fps) + " fps (RGB)"
  };

  trossen::demo::print_config_banner("Unimanual AI Kit with RealSense Demo", config_lines);

  // Install signal handler for graceful shutdown
  trossen::demo::install_signal_handler();

  // Create root directory
  std::filesystem::create_directories(cfg.root);

  // ──────────────────────────────────────────────────────────
  // Initialize hardware
  // ──────────────────────────────────────────────────────────

  std::shared_ptr<trossen_arm::TrossenArmDriver> leader_driver;
  std::shared_ptr<trossen_arm::TrossenArmDriver> follower_driver;

  std::shared_ptr<trossen::hw::HardwareComponent> leader_component;
  std::shared_ptr<trossen::hw::HardwareComponent> follower_component;

  const float moving_time_s = 2.0f;

  std::cout << "Initializing hardware...\n";

  // Create leader arm via registry
  nlohmann::json leader_hw_cfg = {
    {"ip_address", cfg.leader_ip},
    {"model", "wxai_v0"},
    {"end_effector", "wxai_v0_leader"}
  };
  leader_component = trossen::hw::HardwareRegistry::create(
    "trossen_arm", "leader", leader_hw_cfg, true);
  auto leader_comp_typed = std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(
    leader_component);
  leader_driver = leader_comp_typed->get_hardware();
  std::cout << "  ✓ Leader configured (" << cfg.leader_ip << ")\n";

  // Create follower arm via registry
  nlohmann::json follower_hw_cfg = {
    {"ip_address", cfg.follower_ip},
    {"model", "wxai_v0"},
    {"end_effector", "wxai_v0_follower"}
  };
  follower_component = trossen::hw::HardwareRegistry::create(
    "trossen_arm", "follower", follower_hw_cfg, true);
  auto follower_comp_typed =
    std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(
      follower_component);
  follower_driver = follower_comp_typed->get_hardware();
  std::cout << "  ✓ Follower configured (" << cfg.follower_ip << ")\n";

  // Stage both arms to starting positions
  leader_driver->set_all_modes(trossen_arm::Mode::position);
  follower_driver->set_all_modes(trossen_arm::Mode::position);

  leader_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);
  follower_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);

  std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
  std::cout << "  ✓ Arms staged to starting positions\n";

  // Adjust follower joint limits for gripper tolerance
  auto limits = follower_driver->get_joint_limits();
  limits[follower_driver->get_num_joints() - 1].position_tolerance = 0.01;
  follower_driver->set_joint_limits(limits);

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

  // Joint state producers for both arms (leader + follower)
  std::vector<std::shared_ptr<trossen::hw::PolledProducer>> joint_producers;

  // Create producer for leader via registry
  nlohmann::json leader_prod_cfg = {
    {"stream_id", "leader"},
    {"use_device_time", false}
  };
  auto leader_prod = trossen::runtime::ProducerRegistry::create(
    "trossen_arm", leader_component, leader_prod_cfg);
  joint_producers.push_back(leader_prod);
  mgr.add_producer(leader_prod, joint_period);
  std::cout << "  ✓ Leader producer (" << cfg.joint_rate_hz << " Hz)\n";

  // Create producer for follower via registry
  nlohmann::json follower_prod_cfg = {
    {"stream_id", "follower"},
    {"use_device_time", false}
  };
  auto follower_prod = trossen::runtime::ProducerRegistry::create(
    "trossen_arm", follower_component, follower_prod_cfg);
  joint_producers.push_back(follower_prod);
  mgr.add_producer(follower_prod, joint_period);
  std::cout << "  ✓ Follower producer (" << cfg.joint_rate_hz << " Hz)\n";

  // ──────────────────────────────────────────────────────────
  // RealSense Camera producers (RGB only) - 2 cameras
  // ──────────────────────────────────────────────────────────
  auto camera_period = std::chrono::milliseconds(static_cast<int>(1000.0f / cfg.camera_fps));

  std::vector<std::shared_ptr<trossen::hw::HardwareComponent>> camera_components;
  std::vector<std::shared_ptr<trossen::hw::PolledProducer>> camera_producers;

  for (size_t i = 0; i < cfg.camera_serials.size(); ++i) {
    // Create hardware component for RealSense camera
    nlohmann::json camera_hw_cfg = {
      {"serial_number", cfg.camera_serials[i]},
      {"width", cfg.camera_width},
      {"height", cfg.camera_height},
      {"fps", cfg.camera_fps},
      {"use_depth", false},
      {"force_hardware_reset", false}
    };

    auto camera_component = trossen::hw::HardwareRegistry::create(
      "realsense_camera", "camera_" + std::to_string(i), camera_hw_cfg);
    camera_components.push_back(camera_component);
    std::cout << "  ✓ RealSense camera " << i << " hardware initialized ("
              << cfg.camera_serials[i] << ")\n";

    // Create RGB producer
    nlohmann::json rgb_prod_cfg = {
      {"stream_id", "camera_" + std::to_string(i)},
      {"encoding", "bgr8"},
      {"use_device_time", true},
      {"width", cfg.camera_width},
      {"height", cfg.camera_height},
      {"fps", cfg.camera_fps}
    };

    auto rgb_prod = trossen::runtime::ProducerRegistry::create(
      "realsense_camera", camera_component, rgb_prod_cfg);
    camera_producers.push_back(rgb_prod);
    mgr.add_producer(rgb_prod, camera_period);
    std::cout << "  ✓ RealSense camera " << i << " producer (" << cfg.camera_fps << " Hz, "
              << cfg.camera_width << "x" << cfg.camera_height << " RGB)\n";
  }

  std::cout << "\nProducers registered. Ready to record.\n";
  std::cout << "  Recording: 2 arms (leader + follower) + 2 RealSense cameras\n";

  while (true) {
    if (trossen::demo::g_stop_requested) {
      std::cout << "\n\nStopping at user request (Ctrl+C).\n";
      break;
    }

    // Lock the leader arm, move follower to match
    if (leader_driver && follower_driver) {
      std::cout << "\nLocking leader arm and moving follower to match...\n";

      // Lock leader in position mode
      leader_driver->set_all_modes(trossen_arm::Mode::position);

      // Get current leader positions
      auto leader_positions = leader_driver->get_all_positions();

      // Move follower to match
      follower_driver->set_all_modes(trossen_arm::Mode::position);
      follower_driver->set_all_positions(leader_positions, moving_time_s, false);
      std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
      std::cout << "  ✓ Follower moved to match leader\n";

      // Re-enable teleop on leader
      std::cout << "\n!! Enabling teleop mode !!\n";
      std::cout << "   Leader: gravity compensation (external_effort mode)\n";
      std::cout << "   Follower: will mirror Leader\n";
      std::cout << "   Both arms will be recorded\n\n";
      leader_driver->set_all_modes(trossen_arm::Mode::external_effort);
      leader_driver->set_all_external_efforts(
        std::vector<double>(leader_driver->get_num_joints(), 0.0),
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

    // Teleop loop - mirror leader positions to follower
    std::thread teleop_thread([&]() {
      while (mgr.is_episode_active() && !trossen::demo::g_stop_requested) {
        // Mirror leader to follower
        auto leader_js = leader_driver->get_all_positions();
        follower_driver->set_all_positions(leader_js, 0.0f, false);

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
      2,  // 2 joint producers (leader + follower)
      cfg.joint_rate_hz,
      2,  // 2 RealSense cameras (RGB only)
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
  if (leader_driver && follower_driver) {
    std::cout << "\nReturning arms to starting positions...\n";

    leader_driver->set_all_modes(trossen_arm::Mode::position);
    follower_driver->set_all_modes(trossen_arm::Mode::position);

    leader_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);
    follower_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);

    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));

    // Move to sleep positions
    std::cout << "Moving arms to sleep positions...\n";
    leader_driver->set_all_positions(
      std::vector<double>(leader_driver->get_num_joints(), 0.0),
      moving_time_s,
      false);
    follower_driver->set_all_positions(
      std::vector<double>(follower_driver->get_num_joints(), 0.0),
      moving_time_s,
      false);
    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
  }

  auto final_stats = mgr.stats();
  std::vector<std::string> extra_info = {
    "Data streams:         2 arms (both recorded) + 2 RealSense cameras (RGB)"
  };
  trossen::demo::print_final_summary(
    final_stats.total_episodes_completed,
    cfg.root,
    extra_info);

  return 0;
}
