/**
 * @file widowxai_bimanual.cpp
 * @brief Complete Stationary AI Kit demo with Session Manager, MCAP backend, and multi-camera
 *
 * This demo is designed for the full stationary AI kit configuration:
 * - 4 Trossen AI arms (2 leaders for teleop, 2 followers recorded)
 * - Session Manager for multi-episode recording
 * - MCAP backend for data storage
 * - 4 OpenCV camera producers for image capture
 * - Configurable episode count and duration
 *
 * Hardware Configuration:
 *   Leader Left:    192.168.1.3 (teleop control, not recorded)
 *   Leader Right:   192.168.1.2 (teleop control, not recorded)
 *   Follower Left:  192.168.1.5 (mirrors leader left, recorded)
 *   Follower Right: 192.168.1.4 (mirrors leader right, recorded)
 *   Cameras:        4x 720p @ 30fps (indices 0, 2, 4, 6)
 *
 * Usage:
 *   ./widowxai_bimanual --episodes 5 --duration 10
 *   ./widowxai_bimanual --episodes 3 --duration 5 --root /data/recordings
 *   ./widowxai_bimanual --mock  # Use mock producers for testing without hardware
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
#include "trossen_sdk/hw/arm/mock_joint_producer.hpp"
#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/camera/mock_producer.hpp"
#include "trossen_sdk/hw/camera/opencv_producer.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"

#include "./demo_utils.hpp"

struct Config {
  int duration_s = 10;
  int episodes = 3;
  std::string dataset_id = "";  // empty = auto-generate
  std::string root = trossen::io::backends::get_default_root_path().string();
  bool use_mock = false;
  bool show_help = false;

  // Camera settings
  std::vector<int> camera_indices = {4, 10, 16, 22};
  int camera_width = 1280;   // 720p
  int camera_height = 720;
  int camera_fps = 30;

  // Arm settings
  float joint_rate_hz = 200.0f;
  std::string leader_left_ip = "192.168.1.3";
  std::string leader_right_ip = "192.168.1.2";
  std::string follower_left_ip = "192.168.1.5";
  std::string follower_right_ip = "192.168.1.4";

  // Dataset backend type
  std::string backend_type = "mcap";
};

void print_usage(const char* prog_name) {
  std::cout
    << "Usage: " << prog_name << " [options]\n\n"
    << "Options:\n"
    << "  --dataset-id <string>    Dataset identifier (default: auto-generate UUID)\n"
    << "  --root <path>            Root directory for episodes (default: ~/.cache/trossen_sdk/)\n"
    << "  --mock                   Use mock producers instead of real hardware\n"
    << "  --camera-width <pixels>  Camera width (default: 1280 for 720p)\n"
    << "  --camera-height <pixels> Camera height (default: 720)\n"
    << "  --camera-fps <fps>       Camera frame rate (default: 30)\n"
    << "  --joint-rate <hz>        Joint state capture rate (default: 200)\n"
    << "  --backend <type>         Dataset backend type (default: mcap)\n"
    << "  --help                   Show this help message\n\n"
    << "Hardware Configuration:\n"
    << "  Leader Left:    192.168.1.3 (teleop, not recorded)\n"
    << "  Leader Right:   192.168.1.2 (teleop, not recorded)\n"
    << "  Follower Left:  192.168.1.5 (recorded)\n"
    << "  Follower Right: 192.168.1.4 (recorded)\n"
    << "  Cameras:        /dev/video0, /dev/video2, /dev/video4, /dev/video6\n\n"
    << "Examples:\n"
    << "  " << prog_name << "\n"
    << "  " << prog_name << " --mock\n"
    << "  " << prog_name << " --dataset-id stationary_demo_001 --root /data/recordings\n";
}

Config parse_args(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      cfg.show_help = true;
      return cfg;
    } else if (arg == "--duration" && i + 1 < argc) {
      cfg.duration_s = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--episodes" && i + 1 < argc) {
      cfg.episodes = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--dataset-id" && i + 1 < argc) {
      cfg.dataset_id = argv[++i];
    } else if (arg == "--root" && i + 1 < argc) {
      cfg.root = argv[++i];
    } else if (arg == "--mock") {
      cfg.use_mock = true;
    } else if (arg == "--camera-width" && i + 1 < argc) {
      cfg.camera_width = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--camera-height" && i + 1 < argc) {
      cfg.camera_height = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--camera-fps" && i + 1 < argc) {
      cfg.camera_fps = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--joint-rate" && i + 1 < argc) {
      cfg.joint_rate_hz = std::max(1.0f, static_cast<float>(std::atof(argv[++i])));
    } else if (arg == "--backend" && i + 1 < argc) {
      cfg.backend_type = argv[++i];
    } else {
      std::cerr << "Warning: Unknown argument '" << arg << "' (use --help for usage)\n";
    }
  }
  return cfg;
}

int main(int argc, char** argv) {
  // Parse command-line arguments
  auto cfg = parse_args(argc, argv);
  if (cfg.show_help) {
    print_usage(argv[0]);
    return 0;
  }

  // Print configuration
  std::vector<std::string> config_lines = {
    "Mode:                 " + std::string(cfg.use_mock ? "Mock (no hardware)" : "Hardware"),
    "Dataset ID:           " + (cfg.dataset_id.empty() ? "<auto-generate>" : cfg.dataset_id),
    "Root directory:       " + cfg.root,
    "Backend:              " + cfg.backend_type
  };

  if (!cfg.use_mock) {
    config_lines.push_back(
      "Leader Left IP:       " + cfg.leader_left_ip + " (teleop, not recorded)");
    config_lines.push_back(
      "Leader Right IP:      " + cfg.leader_right_ip + " (teleop, not recorded)");
    config_lines.push_back(
      "Follower Left IP:     " + cfg.follower_left_ip + " (recorded)");
    config_lines.push_back(
      "Follower Right IP:    " + cfg.follower_right_ip + " (recorded)");
  }

  config_lines.push_back(
    "Joint rate:           " + std::to_string(cfg.joint_rate_hz) + " Hz (followers only)");
  config_lines.push_back(
    "Cameras:              " + std::to_string(cfg.camera_indices.size()) + " cameras @ " +
    std::to_string(cfg.camera_width) + "x" + std::to_string(cfg.camera_height) +
    " @ " + std::to_string(cfg.camera_fps) + " fps");

  std::string camera_list = "Camera indices:       ";
  for (size_t i = 0; i < cfg.camera_indices.size(); ++i) {
    camera_list += "/dev/video" + std::to_string(cfg.camera_indices[i]);
    if (i < cfg.camera_indices.size() - 1) camera_list += ", ";
  }
  config_lines.push_back(camera_list);

  trossen::demo::print_config_banner("Stationary AI Kit Complete Demo", config_lines);

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

  std::vector<double> leader_left_starting_pos;
  std::vector<double> leader_right_starting_pos;
  std::vector<double> follower_left_starting_pos;
  std::vector<double> follower_right_starting_pos;

  const float moving_time_s = 2.0f;

  if (!cfg.use_mock) {
    std::cout << "Initializing hardware...\n";

    // Create leader left via registry
    nlohmann::json leader_left_hw_cfg = {
      {"ip_address", cfg.leader_left_ip},
      {"model", "wxai_v0"},
      {"end_effector", "wxai_v0_leader"}
    };
    auto leader_left_component = trossen::hw::HardwareRegistry::create(
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
    auto leader_right_component = trossen::hw::HardwareRegistry::create(
      "trossen_arm", "leader_right", leader_right_hw_cfg, true);
    auto leader_right_comp_typed =
      std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(leader_right_component);
    leader_right_driver = leader_right_comp_typed->get_hardware();
    std::cout << "  ✓ Leader Right configured (" << cfg.leader_right_ip << ")\n";

    // Stage leader arms to starting positions
    leader_left_driver->set_all_modes(trossen_arm::Mode::position);
    leader_right_driver->set_all_modes(trossen_arm::Mode::position);
    leader_left_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);
    leader_right_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);
    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
    std::cout << "  ✓ Leader arms staged to starting positions\n";
  }

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

  // Joint state producers for follower arms
  std::vector<std::shared_ptr<trossen::hw::PolledProducer>> joint_producers;

  if (cfg.use_mock) {
    // Mock follower left
    trossen::hw::arm::MockJointStateProducer::Config left_cfg{
      6,                              // num_joints
      cfg.joint_rate_hz,              // rate_hz
      "follower_left/joint_states",   // stream_id
      1.0                             // motion_scale
    };
    auto left_prod = std::make_shared<trossen::hw::arm::MockJointStateProducer>(left_cfg);
    joint_producers.push_back(left_prod);
    mgr.add_producer(left_prod, joint_period);
    std::cout << "  ✓ Mock follower left producer (" << cfg.joint_rate_hz << " Hz)\n";

    // Mock follower right
    trossen::hw::arm::MockJointStateProducer::Config right_cfg{
      6,                              // num_joints
      cfg.joint_rate_hz,              // rate_hz
      "follower_right/joint_states",  // stream_id
      1.0                             // motion_scale
    };
    auto right_prod = std::make_shared<trossen::hw::arm::MockJointStateProducer>(right_cfg);
    joint_producers.push_back(right_prod);
    mgr.add_producer(right_prod, joint_period);
    std::cout << "  ✓ Mock follower right producer (" << cfg.joint_rate_hz << " Hz)\n";
  } else {
    // Create follower left via registry
    nlohmann::json left_hw_cfg = {
      {"ip_address", cfg.follower_left_ip},
      {"model", "wxai_v0"},
      {"end_effector", "wxai_v0_follower"}
    };
    auto left_component = trossen::hw::HardwareRegistry::create(
      "trossen_arm", "follower_left", left_hw_cfg, true);
    auto left_comp_typed = std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(
      left_component);
    follower_left_driver = left_comp_typed->get_hardware();
    std::cout << "  ✓ Follower Left configured (" << cfg.follower_left_ip << ")\n";

    // Create follower right via registry
    nlohmann::json right_hw_cfg = {
      {"ip_address", cfg.follower_right_ip},
      {"model", "wxai_v0"},
      {"end_effector", "wxai_v0_follower"}
    };
    auto right_component = trossen::hw::HardwareRegistry::create(
      "trossen_arm", "follower_right", right_hw_cfg, true);
    auto right_comp_typed = std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(
      right_component);
    follower_right_driver = right_comp_typed->get_hardware();
    std::cout << "  ✓ Follower Right configured (" << cfg.follower_right_ip << ")\n";

    // Adjust follower joint limits for gripper tolerance and stage
    auto left_limits = follower_left_driver->get_joint_limits();
    left_limits[follower_left_driver->get_num_joints() - 1].position_tolerance = 0.01;
    follower_left_driver->set_joint_limits(left_limits);
    follower_left_driver->set_all_modes(trossen_arm::Mode::position);
    follower_left_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);

    auto right_limits = follower_right_driver->get_joint_limits();
    right_limits[follower_right_driver->get_num_joints() - 1].position_tolerance = 0.01;
    follower_right_driver->set_joint_limits(right_limits);
    follower_right_driver->set_all_modes(trossen_arm::Mode::position);
    follower_right_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);

    // Wait for both followers to reach position
    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
    std::cout << "  ✓ Followers staged to starting positions\n";

    // Create producers via registry
    nlohmann::json left_prod_cfg = {
      {"stream_id", "follower_left/joint_states"},
      {"use_device_time", false}
    };
    auto left_prod = trossen::runtime::ProducerRegistry::create(
      "trossen_arm", left_component, left_prod_cfg);
    joint_producers.push_back(left_prod);
    mgr.add_producer(left_prod, joint_period);
    std::cout << "  ✓ Follower Left producer (" << cfg.joint_rate_hz << " Hz)\n";

    nlohmann::json right_prod_cfg = {
      {"stream_id", "follower_right/joint_states"},
      {"use_device_time", false}
    };
    auto right_prod = trossen::runtime::ProducerRegistry::create(
      "trossen_arm", right_component, right_prod_cfg);
    joint_producers.push_back(right_prod);
    mgr.add_producer(right_prod, joint_period);
    std::cout << "  ✓ Follower Right producer (" << cfg.joint_rate_hz << " Hz)\n";
  }

  // ──────────────────────────────────────────────────────────
  // Camera producers
  // ──────────────────────────────────────────────────────────
  std::vector<std::shared_ptr<trossen::hw::PolledProducer>> camera_producers;
  auto camera_period = std::chrono::milliseconds(static_cast<int>(1000.0f / cfg.camera_fps));

  for (size_t i = 0; i < cfg.camera_indices.size(); ++i) {
    if (cfg.use_mock) {
      trossen::hw::camera::MockCameraProducer::Config cam_cfg;
      cam_cfg.width = cfg.camera_width;
      cam_cfg.height = cfg.camera_height;
      cam_cfg.fps = cfg.camera_fps;
      cam_cfg.stream_id = "camera_" + std::to_string(i) + "/image";
      cam_cfg.encoding = "bgr8";
      cam_cfg.pattern = trossen::hw::camera::MockCameraProducer::Pattern::Gradient;
      cam_cfg.seed = 1000 + i;
      cam_cfg.warmup_frames = 3;

      auto cam_prod = std::make_shared<trossen::hw::camera::MockCameraProducer>(cam_cfg);
      camera_producers.push_back(cam_prod);
      mgr.add_producer(cam_prod, camera_period);
      std::cout << "  ✓ Mock camera " << i << " producer (" << cfg.camera_fps << " Hz, "
                << cfg.camera_width << "x" << cfg.camera_height << ")\n";
    } else {
      // Create hardware component via registry
      nlohmann::json hw_cfg = {
        {"device_index", cfg.camera_indices[i]},
        {"width", cfg.camera_width},
        {"height", cfg.camera_height},
        {"fps", cfg.camera_fps},
        {"backend", "v4l2"}
      };

      auto camera_component = trossen::hw::HardwareRegistry::create(
        "opencv_camera", "camera_" + std::to_string(i), hw_cfg);

      // Create producer via registry
      nlohmann::json prod_cfg = {
        {"stream_id", "camera_" + std::to_string(i) + "/image"},
        {"encoding", "bgr8"},
        {"use_device_time", false},
        {"width", cfg.camera_width},
        {"height", cfg.camera_height},
        {"fps", cfg.camera_fps}
      };

      auto cam_prod = trossen::runtime::ProducerRegistry::create(
        "opencv_camera", camera_component, prod_cfg);

      camera_producers.push_back(cam_prod);
      mgr.add_producer(cam_prod, camera_period);
      std::cout << "  ✓ Camera " << i << " producer (" << cfg.camera_fps << " Hz, "
                << cfg.camera_width << "x" << cfg.camera_height << ")\n";
    }
  }

  std::cout << "\nProducers registered. Ready to record.\n";
  std::cout << "  Recording: 2 follower arms + 4 cameras = 6 data streams\n";

  while (true) {
    if (trossen::demo::g_stop_requested) {
      std::cout << "\n\nStopping at user request (Ctrl+C).\n";
      break;
    }

    // Lock the leader arms, move followers to match
    if (!cfg.use_mock && leader_left_driver && leader_right_driver &&
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
      std::cout << "   Follower Right: will mirror Leader Right\n\n";
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

    // Teleop loop (if using hardware) - mirror leader positions to followers
    std::thread teleop_thread;
    if (!cfg.use_mock) {
      teleop_thread = std::thread([&]() {
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
    }

    // Blocking monitor call: keeps the main thread alive while the episode is recording,
    // prevents threads from joining before data is collected, and updates/prints status logs.
    trossen::runtime::SessionManager::Stats last_stats = mgr.monitor_episode();

    // Stop episode and wait for teleop thread
    if (mgr.is_episode_active()) {
      mgr.stop_episode();
    }

    if (!cfg.use_mock && teleop_thread.joinable()) {
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
      2,  // 2 joint producers (follower left + follower right)
      cfg.joint_rate_hz,
      static_cast<int>(cfg.camera_indices.size()),  // 4 cameras
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

  // Return arms to starting position (if using hardware)
  if (!cfg.use_mock && leader_left_driver && leader_right_driver &&
      follower_left_driver && follower_right_driver) {
    std::cout << "\nReturning arms to starting positions...\n";

    leader_left_driver->set_all_modes(trossen_arm::Mode::position);
    leader_right_driver->set_all_modes(trossen_arm::Mode::position);

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
    "Data streams:         2 follower arms + "
    + std::to_string(cfg.camera_indices.size()) + " cameras"
  };
  trossen::demo::print_final_summary(
    final_stats.total_episodes_completed,
    cfg.root,
    extra_info);

  return 0;
}
