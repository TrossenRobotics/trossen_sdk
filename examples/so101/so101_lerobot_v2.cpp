/**
 * @file so101_lerobot.cpp
 * @brief Complete SO101 demo with Session Manager, TrossenMCAP backend, and OpenCV camera
 *
 * This demo combines:
 * - SO101 hardware (leader + follower arms)
 * - Session Manager for multi-episode recording
 * - TrossenMCAP backend for data storage
 * - OpenCV camera producer for image capture
 * - Configurable episode count and duration
 *
 * Usage:
 *   ./so101_teleop
 *   ./so101_teleop --root /data/recordings
 *   ./so101_teleop --mock  # Use mock producers for testing without hardware
 */

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "trossen_sdk/hw/arm/so101_arm_driver.hpp"
#include "trossen_sdk/hw/arm/teleop_mock_joint_producer.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"
#include "trossen_sdk/hw/arm/so101_teleop_arm_producer.hpp"
#include "trossen_sdk/hw/camera/opencv_producer.hpp"
#include "trossen_sdk/hw/camera/mock_producer.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"

#include "trossen_sdk/utils/app_utils.hpp"


// ────────────────────────────────────────────────────────────
// Command-line configuration
// ────────────────────────────────────────────────────────────

struct Config {
  int duration_s = 10;
  int episodes = 1;
  std::string dataset_id = "";  // empty = auto-generate
  std::string root = trossen::io::backends::get_default_root_path().string();
  std::string repository_id = "TrossenRoboticsCommunity";  // Valid only for LeRobotV2 backend
  bool use_mock = false;
  bool show_help = false;

  // Camera settings
  int camera_index = 4;
  int camera_width = 1920;
  int camera_height = 1080;
  int camera_fps = 30;

  // Arm settings
  float joint_rate_hz = 30.0f;
  std::string leader_port = "/dev/ttyACM1";
  std::string follower_port = "/dev/ttyACM0";

  // Dataset backend type
  std::string backend_type = "trossen_mcap";
};

void print_usage(const char* prog_name) {
  std::cout
    << "Usage: " << prog_name << " [options]\n\n"
    << "Options:\n"
    << "  --duration <seconds>     Duration per episode (default: 10)\n"
    << "  --episodes <count>       Number of episodes to record (default: 1)\n"
    << "  --dataset-id <string>    Dataset identifier (default: auto-generate UUID)\n"
    << "  --root <path>            Root directory for episodes (default: ~/.cache/trossen_sdk/)\n"
    << "  --repository-id <string> Repository identifier (default: TrossenRoboticsCommunity, "
    << "only for LeRobotV2 backend)\n"
    << "  --mock                   Use mock producers instead of real hardware\n"
    << "  --camera-index <num>     Camera device index (default: 4, i.e., /dev/video4)\n"
    << "  --camera-width <pixels>  Camera width (default: 1920)\n"
    << "  --camera-height <pixels> Camera height (default: 1080)\n"
    << "  --camera-fps <fps>       Camera frame rate (default: 30)\n"
    << "  --joint-rate <hz>        Joint state capture rate (default: 30)\n"
    << "  --leader-port <port>     Leader arm serial port (default: /dev/ttyACM1)\n"
    << "  --follower-port <port>   Follower arm serial port (default: /dev/ttyACM0)\n"
    << "  --backend <type>         Dataset backend type (default: mcap)\n"
    << "  --help                   Show this help message\n\n"
    << "Examples:\n"
    << "  " << prog_name << " --duration 10 --episodes 5\n"
    << "  " << prog_name << " --mock --duration 5 --episodes 3\n"
    << "  " << prog_name << " --dataset-id so101_demo_001 --root /data/recordings\n";
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
    } else if (arg == "--repository-id" && i + 1 < argc) {
      cfg.repository_id = argv[++i];
    } else if (arg == "--mock") {
      cfg.use_mock = true;
    } else if (arg == "--camera-index" && i + 1 < argc) {
      cfg.camera_index = std::atoi(argv[++i]);
    } else if (arg == "--camera-width" && i + 1 < argc) {
      cfg.camera_width = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--camera-height" && i + 1 < argc) {
      cfg.camera_height = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--camera-fps" && i + 1 < argc) {
      cfg.camera_fps = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--joint-rate" && i + 1 < argc) {
      cfg.joint_rate_hz = std::max(1.0f, static_cast<float>(std::atof(argv[++i])));
    } else if (arg == "--leader-port" && i + 1 < argc) {
      cfg.leader_port = argv[++i];
    } else if (arg == "--follower-port" && i + 1 < argc) {
      cfg.follower_port = argv[++i];
    } else if (arg == "--backend" && i + 1 < argc) {
      cfg.backend_type = argv[++i];
    } else {
      std::cerr << "Warning: Unknown argument '" << arg << "' (use --help for usage)\n";
    }
  }
  return cfg;
}

// ────────────────────────────────────────────────────────────
// Main
// ────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  // Parse command-line arguments
  auto cfg = parse_args(argc, argv);
  if (cfg.show_help) {
    print_usage(argv[0]);
    return 0;
  }
  // TODO(so101): This example is not yet fully functional. It needs to be migrated to use
  // examples/so101/config.json with SdkConfig::from_json() following the pattern
  // in examples/trossen_solo_ai/, examples/trossen_stationary_ai/, and examples/trossen_mobile_ai/.
  // The hand-rolled CLI parser above should also be replaced with
  // trossen::configuration::CliParser.
  std::cerr << "Error: so101_teleop is not yet functional.\n"
            << "See examples/so101/so101_lerobot_v2.cpp for the migration TODO.\n";
  return 1;

  // Print configuration
  std::vector<std::string> config_lines = {
    "Mode:                 " + std::string(cfg.use_mock ? "Mock (no hardware)" : "Hardware"),
    "Duration per episode: " + std::to_string(cfg.duration_s) + "s",
    "Number of episodes:   " + std::to_string(cfg.episodes),
    "Dataset ID:           " + (cfg.dataset_id.empty() ? "<auto-generate>" : cfg.dataset_id),
    "Root directory:       " + cfg.root,
    "Repository ID:        " + cfg.repository_id,
    "Backend:              " + cfg.backend_type
  };

  if (!cfg.use_mock) {
    config_lines.push_back("Leader Port:          " + cfg.leader_port);
    config_lines.push_back("Follower Port:        " + cfg.follower_port);
  }

  config_lines.push_back("Joint rate:           " + std::to_string(cfg.joint_rate_hz) + " Hz");
  config_lines.push_back(
    "Camera:               /dev/video" + std::to_string(cfg.camera_index) +
    " @ " + std::to_string(cfg.camera_width) + "x" + std::to_string(cfg.camera_height) +
    " @ " + std::to_string(cfg.camera_fps) + " fps");

  trossen::utils::print_config_banner("SO101 LeRobotV2 Complete Demo", config_lines);

  // Install signal handler for graceful shutdown
  trossen::utils::install_signal_handler();

  // Create root directory
  std::filesystem::create_directories(cfg.root);

  // ──────────────────────────────────────────────────────────
  // Initialize hardware (if not using mock)
  // ──────────────────────────────────────────────────────────

  std::shared_ptr<SO101ArmDriver> leader_driver;
  std::shared_ptr<SO101ArmDriver> follower_driver;

  if (!cfg.use_mock) {
    std::cout << "Initializing hardware...\n";

    // Create and configure leader driver
    leader_driver = std::make_shared<SO101ArmDriver>();
    if (!leader_driver->configure(SO101EndEffector::leader, cfg.leader_port)) {
      std::cerr << "Failed to configure leader on " << cfg.leader_port << "\n";
      return 1;
    }

    // Create and configure follower driver
    follower_driver = std::make_shared<SO101ArmDriver>();
    if (!follower_driver->configure(SO101EndEffector::follower, cfg.follower_port)) {
      std::cerr << "Failed to configure follower on " << cfg.follower_port << "\n";
      return 1;
    }

    // Connect to leader
    if (!leader_driver->connect()) {
      std::cerr << "Failed to connect to leader on " << cfg.leader_port << "\n";
      return 1;
    }
    std::cout << "  [ok] Leader arm connected (" << cfg.leader_port << ")\n";

    // Connect to follower
    if (!follower_driver->connect()) {
      std::cerr << "Failed to connect to follower on " << cfg.follower_port << "\n";
      return 1;
    }
    std::cout << "  [ok] Follower arm connected (" << cfg.follower_port << ")\n";
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

  // Joint state producer (arm or mock)
  std::shared_ptr<trossen::hw::PolledProducer> joint_producer;
  if (cfg.use_mock) {
    trossen::hw::arm::TeleopMockJointStateProducer::Config joint_cfg{
      6,                              // num_joints
      cfg.joint_rate_hz,              // rate_hz
      "teleop_robot/joint_states",    // stream_id
      1.0                             // motion_scale
    };
    joint_producer = std::make_shared<trossen::hw::arm::TeleopMockJointStateProducer>(joint_cfg);
    std::cout << "  [ok] Mock joint state producer (" << cfg.joint_rate_hz << " Hz, 6 joints)\n";
  } else {
    trossen::hw::arm::TeleopSO101ArmProducer::Config joint_cfg;
    joint_cfg.stream_id = "teleop_robot/joint_states";
    joint_cfg.use_device_time = false;

    joint_producer = std::make_shared<trossen::hw::arm::TeleopSO101ArmProducer>(
      leader_driver, follower_driver, joint_cfg);
    std::cout << "  [ok] SO101 arm producer (" << cfg.joint_rate_hz << " Hz)\n";
  }

  auto joint_period = std::chrono::milliseconds(static_cast<int>(1000.0f / cfg.joint_rate_hz));
  mgr.add_producer(joint_producer, joint_period);

  // ──────────────────────────────────────────────────────────
  // Camera producer
  // ──────────────────────────────────────────────────────────
  std::shared_ptr<trossen::hw::PolledProducer> camera_producer;
  if (cfg.use_mock) {
    trossen::hw::camera::MockCameraProducer::Config cam_cfg;
    cam_cfg.width = cfg.camera_width;
    cam_cfg.height = cfg.camera_height;
    cam_cfg.fps = cfg.camera_fps;
    cam_cfg.stream_id = "camera_main";
    cam_cfg.encoding = "bgr8";
    cam_cfg.pattern = trossen::hw::camera::MockCameraProducer::Pattern::Gradient;
    cam_cfg.warmup_frames = 3;
    camera_producer = std::make_shared<trossen::hw::camera::MockCameraProducer>(cam_cfg);
    std::cout << "  [ok] Mock camera producer (" << cfg.camera_fps << " Hz, "
              << cfg.camera_width << "x" << cfg.camera_height << ")\n";
  } else {
    // Create hardware component via registry
    nlohmann::json hw_cfg = {
      {"device_index", cfg.camera_index},
      {"width", cfg.camera_width},
      {"height", cfg.camera_height},
      {"fps", cfg.camera_fps},
      {"backend", "v4l2"}
    };

    auto camera_component = trossen::hw::HardwareRegistry::create(
      "opencv_camera", "camera_main", hw_cfg);

    // Create producer via registry
    nlohmann::json prod_cfg = {
      {"stream_id", "camera_main"},
      {"encoding", "bgr8"},
      {"use_device_time", false},
      {"width", cfg.camera_width},
      {"height", cfg.camera_height},
      {"fps", cfg.camera_fps}
    };

    camera_producer = trossen::runtime::ProducerRegistry::create(
      "opencv_camera", camera_component, prod_cfg);

    std::cout << "  [ok] OpenCV camera producer (" << cfg.camera_fps << " Hz, "
              << cfg.camera_width << "x" << cfg.camera_height << ")\n";
  }

  auto camera_period = std::chrono::milliseconds(static_cast<int>(1000.0f / cfg.camera_fps));
  mgr.add_producer(camera_producer, camera_period);

  std::cout << "\nProducers registered. Ready to record.\n";

  // ──────────────────────────────────────────────────────────
  // Ready for teleoperation
  // ──────────────────────────────────────────────────────────

  if (!cfg.use_mock) {
    std::cout << "\n!! SO101 arms ready for teleoperation !!\n";
    std::cout << "   Leader: will read positions from operator\n";
    std::cout << "   Follower: will mirror leader positions\n\n";
  }

  // ──────────────────────────────────────────────────────────
  // Recording loop: record requested number of episodes
  // ──────────────────────────────────────────────────────────

  for (int ep = 0; ep < cfg.episodes; ++ep) {
    if (trossen::utils::g_stop_requested) {
      std::cout << "\n\nStopping at user request (Ctrl+C).\n";
      break;
    }

    // Display episode header
    mgr.print_episode_header();

    // Sync follower to leader's current position before recording
    if (!cfg.use_mock) {
      auto leader_positions = leader_driver->get_joint_positions();
      follower_driver->set_joint_positions(leader_positions);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Start episode
    if (!mgr.start_episode()) {
      break;
    }

    // Capture the episode index that's currently recording
    uint32_t recording_episode_index = mgr.stats().current_episode_index;

    // Track episode start time for actual duration calculation
    auto episode_start_time = std::chrono::steady_clock::now();

    std::cout << "Recording...\n";

    // Teleop loop (if using hardware)
    std::thread teleop_thread;
    if (!cfg.use_mock) {
      teleop_thread = std::thread([&]() {
        while (mgr.is_episode_active() && !trossen::utils::g_stop_requested) {
          auto leader_positions = leader_driver->get_joint_positions();
          follower_driver->set_joint_positions(leader_positions);
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
    std::string file_path = trossen::utils::generate_episode_path(
      cfg.root,
      recording_episode_index);
    trossen::utils::print_episode_summary(file_path, last_stats);

    // ──────────────────────────────────────────────────────────
    // Sanity check: verify expected record counts
    // ──────────────────────────────────────────────────────────

    trossen::utils::SanityCheckConfig sanity_cfg{
      last_stats.elapsed.count(),
      1,  // 1 joint producer (follower)
      cfg.joint_rate_hz,
      1,  // 1 camera
      cfg.camera_fps,
      5.0  // 5% tolerance
    };
    trossen::utils::perform_sanity_check(
      recording_episode_index,
      last_stats.records_written_current,
      sanity_cfg);

    // Check if user requested stop
    if (trossen::utils::g_stop_requested) {
      std::cout << "\nStopping at user request (Ctrl+C).\n";
      break;
    }

    // Pause between episodes (unless this was the last one)
    if (ep < cfg.episodes - 1) {
      std::cout << "\nPausing for 1 second before next episode...\n";
      if (!trossen::utils::interruptible_sleep(std::chrono::seconds(1))) {
        break;  // Stop requested during pause
      }
    }
  }

  // ──────────────────────────────────────────────────────────
  // Shutdown and final summary
  // ──────────────────────────────────────────────────────────

  mgr.shutdown();

  // Disconnect SO101 arms
  if (!cfg.use_mock && leader_driver && follower_driver) {
    std::cout << "\nDisconnecting SO101 arms...\n";
    leader_driver->disconnect();
    follower_driver->disconnect();
    std::cout << "  [ok] Arms disconnected\n";
  }

  auto final_stats = mgr.stats();
  trossen::utils::print_final_summary(final_stats.total_episodes_completed, cfg.root);

  return 0;
}
