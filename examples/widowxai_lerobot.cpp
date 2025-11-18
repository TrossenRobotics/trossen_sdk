/**
 * @file trossen_ai_solo_complete.cpp
 * @brief Complete Trossen AI Solo demo with Session Manager, MCAP backend, and OpenCV camera
 *
 * This demo combines:
 * - Trossen AI Solo hardware (leader + follower arms)
 * - Session Manager for multi-episode recording
 * - MCAP backend for data storage
 * - OpenCV camera producer for image capture
 * - Configurable episode count and duration
 *
 * Usage:
 *   ./trossen_ai_solo_complete --episodes 5 --duration 10
 *   ./trossen_ai_solo_complete --episodes 3 --duration 5 --output-dir /data/recordings
 *   ./trossen_ai_solo_complete --mock  # Use mock producers for testing without hardware
 */

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "demo_utils.hpp"
#include "libtrossen_arm/trossen_arm.hpp"
#include "trossen_sdk/hw/arm/teleop_mock_joint_producer.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"
#include "trossen_sdk/hw/arm/teleop_arm_producer.hpp"
#include "trossen_sdk/hw/arm/teleop_mock_joint_producer.hpp"
#include "trossen_sdk/hw/camera/opencv_producer.hpp"
#include "trossen_sdk/hw/camera/mock_producer.hpp"

using namespace std::chrono_literals;
using namespace trossen::demo;

// ────────────────────────────────────────────────────────────
// Command-line configuration
// ────────────────────────────────────────────────────────────

struct Config {
  int duration_s = 10;
  int episodes = 3;
  std::string dataset_id = "";  // empty = auto-generate
  std::string output_dir = "output/episodes";
  bool use_mock = false;
  bool show_help = false;

  // Camera settings
  int camera_index = 2;
  int camera_width = 1920;
  int camera_height = 1080;
  int camera_fps = 30;

  // Arm settings
  float joint_rate_hz = 30.0f;
  std::string leader_ip = "192.168.1.2";
  std::string follower_ip = "192.168.1.4";

  // Dataset backend type
  std::string backend_type = "mcap";
};

void print_usage(const char* prog_name) {
  std::cout << "Usage: " << prog_name << " [options]\n\n"
            << "Options:\n"
            << "  --duration <seconds>     Duration per episode (default: 10)\n"
            << "  --episodes <count>       Number of episodes to record (default: 3)\n"
            << "  --dataset-id <string>    Dataset identifier (default: auto-generate UUID)\n"
            << "  --output-dir <path>      Output directory for episodes (default: output/episodes)\n"
            << "  --mock                   Use mock producers instead of real hardware\n"
            << "  --camera-index <num>     Camera device index (default: 2, i.e., /dev/video2)\n"
            << "  --camera-width <pixels>  Camera width (default: 1920)\n"
            << "  --camera-height <pixels> Camera height (default: 1080)\n"
            << "  --camera-fps <fps>       Camera frame rate (default: 30)\n"
            << "  --joint-rate <hz>        Joint state capture rate (default: 30)\n"
            << "  --leader-ip <ip>         Leader arm IP (default: 192.168.1.2)\n"
            << "  --follower-ip <ip>       Follower arm IP (default: 192.168.1.4)\n"
            << "  --backend <type>         Dataset backend type (default: mcap)\n"
            << "  --help                   Show this help message\n\n"
            << "Examples:\n"
            << "  " << prog_name << " --duration 10 --episodes 5\n"
            << "  " << prog_name << " --mock --duration 5 --episodes 3\n"
            << "  " << prog_name << " --dataset-id solo_demo_001 --output-dir /data/recordings\n";
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
    } else if (arg == "--output-dir" && i + 1 < argc) {
      cfg.output_dir = argv[++i];
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
    } else if (arg == "--leader-ip" && i + 1 < argc) {
      cfg.leader_ip = argv[++i];
    } else if (arg == "--follower-ip" && i + 1 < argc) {
      cfg.follower_ip = argv[++i];
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

  // Print configuration
  std::vector<std::string> config_lines = {
    "Mode:                 " + std::string(cfg.use_mock ? "Mock (no hardware)" : "Hardware"),
    "Duration per episode: " + std::to_string(cfg.duration_s) + "s",
    "Number of episodes:   " + std::to_string(cfg.episodes),
    "Dataset ID:           " + (cfg.dataset_id.empty() ? "<auto-generate>" : cfg.dataset_id),
    "Output directory:     " + cfg.output_dir,
    "Backend:              " + cfg.backend_type
  };

  if (!cfg.use_mock) {
    config_lines.push_back("Leader IP:            " + cfg.leader_ip);
    config_lines.push_back("Follower IP:          " + cfg.follower_ip);
  }

  config_lines.push_back("Joint rate:           " + std::to_string(cfg.joint_rate_hz) + " Hz");
  config_lines.push_back("Camera:               /dev/video" + std::to_string(cfg.camera_index) +
                        " @ " + std::to_string(cfg.camera_width) + "x" + std::to_string(cfg.camera_height) +
                        " @ " + std::to_string(cfg.camera_fps) + " fps");

  print_config_banner("Trossen AI Solo Complete Demo", config_lines);

  // Install signal handler for graceful shutdown
  install_signal_handler();

  // Create output directory
  std::filesystem::create_directories(cfg.output_dir);

  // ──────────────────────────────────────────────────────────
  // Initialize hardware (if not using mock)
  // ──────────────────────────────────────────────────────────

  std::unique_ptr<trossen_arm::TrossenArmDriver> leader_driver;
  std::unique_ptr<trossen_arm::TrossenArmDriver> follower_driver;
  const float moving_time_s = 2.0f;

  if (!cfg.use_mock) {
    std::cout << "Initializing hardware...\n";

    leader_driver = std::make_unique<trossen_arm::TrossenArmDriver>();
    follower_driver = std::make_unique<trossen_arm::TrossenArmDriver>();

    try {
      leader_driver->configure(
        trossen_arm::Model::wxai_v0,
        trossen_arm::StandardEndEffector::wxai_v0_leader,
        cfg.leader_ip,
        true);
      std::cout << "  ✓ Leader arm configured (" << cfg.leader_ip << ")\n";
    } catch (const std::exception& e) {
      std::cerr << "Failed to configure leader: " << e.what() << "\n";
      return 1;
    }

    try {
      follower_driver->configure(
        trossen_arm::Model::wxai_v0,
        trossen_arm::StandardEndEffector::wxai_v0_follower,
        cfg.follower_ip,
        true);
      std::cout << "  ✓ Follower arm configured (" << cfg.follower_ip << ")\n";
    } catch (const std::exception& e) {
      std::cerr << "Failed to configure follower: " << e.what() << "\n";
      return 1;
    }

    // Adjust follower joint limits for gripper tolerance
    auto joint_limits = follower_driver->get_joint_limits();
    joint_limits[follower_driver->get_num_joints() - 1].position_tolerance = 0.01;
    follower_driver->set_joint_limits(joint_limits);    // Set both arms to position mode and stage them

    // Move arms to staged positions
    leader_driver->set_all_modes(trossen_arm::Mode::position);
    follower_driver->set_all_modes(trossen_arm::Mode::position);
    leader_driver->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
    follower_driver->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));

    std::cout << "  ✓ Arms staged to starting positions\n";
  }

  // ──────────────────────────────────────────────────────────
  // Configure Session Manager
  // ──────────────────────────────────────────────────────────

  trossen::runtime::SessionConfig session_cfg;
  session_cfg.base_path = cfg.output_dir;
  session_cfg.dataset_id = cfg.dataset_id;
  session_cfg.max_duration = std::chrono::seconds(cfg.duration_s);
  session_cfg.max_episodes = cfg.episodes;

  if(cfg.backend_type == "mcap") {
    auto mcap_cfg = std::make_unique<trossen::io::backends::McapBackend::Config>();
    mcap_cfg->compression = "zstd";
    mcap_cfg->chunk_size_bytes = 4 * 1024 * 1024;  // 4 MB chunks
    mcap_cfg->robot_name = "/robots/widowxai";
    mcap_cfg->type = "mcap";
    session_cfg.backend_config = std::move(mcap_cfg);
  } else if (cfg.backend_type == "lerobot") {
    auto lerobot_cfg = std::make_unique<trossen::io::backends::LeRobotBackend::Config>();
    lerobot_cfg->output_dir = cfg.output_dir;
    lerobot_cfg->task_name = "trossen_ai_solo_demo";
    lerobot_cfg->repository_id = "TrossenRoboticsCommunity";
    lerobot_cfg->dataset_id = "trossen_ai_solo_dataset";
    lerobot_cfg->overwrite_existing = false;
    lerobot_cfg->encode_videos = true;
    lerobot_cfg->type = "lerobot";
    lerobot_cfg->fps = 30.0f;
    lerobot_cfg->robot_name = "bimanual_widowxai";
    session_cfg.backend_config = std::move(lerobot_cfg);
    
  } else {
    std::cerr << "Unsupported backend type: " << cfg.backend_type << "\n";
    return 1;
  }

  trossen::runtime::SessionManager mgr(std::move(session_cfg));

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
      "teleop_robot/joint_states",        // stream_id
      1.0                             // motion_scale
    };
    joint_producer = std::make_shared<trossen::hw::arm::TeleopMockJointStateProducer>(joint_cfg);
    std::cout << "  ✓ Mock joint state producer (" << cfg.joint_rate_hz << " Hz, 6 joints)\n";
  } else {
    trossen::hw::arm::TeleopTrossenArmProducer::Config joint_cfg;
    joint_cfg.stream_id = "teleop_robot/joint_states";
    joint_cfg.use_device_time = false;

    // Wrap follower_driver in shared_ptr (non-owning wrapper since we manage lifetime)
    auto follower_shared = std::shared_ptr<trossen_arm::TrossenArmDriver>(
      follower_driver.get(), [](trossen_arm::TrossenArmDriver*){});

    auto leader_shared = std::shared_ptr<trossen_arm::TrossenArmDriver>(
      leader_driver.get(), [](trossen_arm::TrossenArmDriver*){});

    joint_producer = std::make_shared<trossen::hw::arm::TeleopTrossenArmProducer>(
      leader_shared, follower_shared, joint_cfg);
    std::cout << "  ✓ Follower arm producer (" << cfg.joint_rate_hz << " Hz)\n";
  }

  auto joint_period = std::chrono::milliseconds(static_cast<int>(1000.0f / cfg.joint_rate_hz));
  mgr.add_producer(joint_producer, joint_period);

  // Camera producer (OpenCV or mock)
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
    std::cout << "  ✓ Mock camera producer (" << cfg.camera_fps << " Hz, "
              << cfg.camera_width << "x" << cfg.camera_height << ")\n";
  } else {
    trossen::hw::camera::OpenCvCameraProducer::Config cam_cfg;
    cam_cfg.device_index = cfg.camera_index;
    cam_cfg.stream_id = "camera_main";
    cam_cfg.encoding = "bgr8";
    cam_cfg.width = cfg.camera_width;
    cam_cfg.height = cfg.camera_height;
    cam_cfg.fps = cfg.camera_fps;
    cam_cfg.use_device_time = false;
    cam_cfg.warmup_seconds = 2.0;
    camera_producer = std::make_shared<trossen::hw::camera::OpenCvCameraProducer>(cam_cfg);

    // Warmup camera before registering
    std::cout << "  ⏳ Warming up camera...\n";
    auto opencv_cam = std::static_pointer_cast<trossen::hw::camera::OpenCvCameraProducer>(camera_producer);
    if (!opencv_cam->warmup()) {
      std::cerr << "Failed to warmup camera\n";
      return 1;
    }
    std::cout << "  ✓ OpenCV camera producer (" << cfg.camera_fps << " Hz, "
              << cfg.camera_width << "x" << cfg.camera_height << ")\n";
  }

  auto camera_period = std::chrono::milliseconds(static_cast<int>(1000.0f / cfg.camera_fps));
  mgr.add_producer(camera_producer, camera_period);

  std::cout << "\nProducers registered. Ready to record.\n";

  // ──────────────────────────────────────────────────────────
  // Enable teleop mode (if using hardware)
  // ──────────────────────────────────────────────────────────

  if (!cfg.use_mock) {
    std::cout << "\n!! Enabling teleop mode !!\n";
    std::cout << "   Leader: gravity compensation (external_effort mode)\n";
    std::cout << "   Follower: will mirror leader positions\n\n";

    leader_driver->set_all_modes(trossen_arm::Mode::external_effort);
    leader_driver->set_all_external_efforts(
      std::vector<double>(leader_driver->get_num_joints(), 0.0),
      0.0,
      false);
  }

  // ──────────────────────────────────────────────────────────
  // Recording loop: record requested number of episodes
  // ──────────────────────────────────────────────────────────

  for (int ep = 0; ep < cfg.episodes; ++ep) {
    if (g_stop_requested) {
      std::cout << "\n\nStopping at user request (Ctrl+C).\n";
      break;
    }

    // Display episode header
    print_episode_header(mgr.stats().current_episode_index, cfg.duration_s);

    // Lock the leader arm, move the follower arm to mirror the leader's current position, and
    // unlock the leader
    if (!cfg.use_mock) {
      leader_driver->set_all_modes(trossen_arm::Mode::position);
      auto leader_positions = leader_driver->get_all_positions();
      follower_driver->set_all_positions(leader_positions, moving_time_s, false);
      std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
      leader_driver->set_all_modes(trossen_arm::Mode::external_effort);
      leader_driver->set_all_external_efforts(
        std::vector<double>(leader_driver->get_num_joints(), 0.0),
        0.0,
        false);
    }

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

    // Teleop loop (if using hardware)
    std::thread teleop_thread;
    if (!cfg.use_mock) {
      teleop_thread = std::thread([&]() {
        while (mgr.is_episode_active() && !g_stop_requested) {
          auto leader_js = leader_driver->get_all_positions();
          follower_driver->set_all_positions(leader_js, 0.0f, false);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      });
    }

    // Monitor loop: display stats while episode is active
    uint64_t last_record_count = monitor_episode(mgr);

    // Stop episode and wait for teleop thread
    if (mgr.is_episode_active()) {
      mgr.stop_episode();
    }

    if (!cfg.use_mock && teleop_thread.joinable()) {
      teleop_thread.join();
    }

    // Calculate actual episode duration
    auto episode_end_time = std::chrono::steady_clock::now();
    double actual_duration = std::chrono::duration<double>(episode_end_time - episode_start_time).count();

    // Build the file path and print summary
    std::string file_path = generate_episode_path(cfg.output_dir, recording_episode_index);
    print_episode_summary(file_path, last_record_count);

    // ──────────────────────────────────────────────────────────
    // Sanity check: verify expected record counts
    // ──────────────────────────────────────────────────────────

    SanityCheckConfig sanity_cfg{
      actual_duration,
      1,  // 1 joint producer (follower)
      cfg.joint_rate_hz,
      1,  // 1 camera
      cfg.camera_fps,
      5.0  // 5% tolerance
    };
    perform_sanity_check(recording_episode_index, last_record_count, sanity_cfg);

    // Check if user requested stop
    if (g_stop_requested) {
      std::cout << "\nStopping at user request (Ctrl+C).\n";
      break;
    }

    // Pause between episodes (unless this was the last one)
    if (ep < cfg.episodes - 1) {
      std::cout << "\nPausing for 1 second before next episode...\n";
      if (!interruptible_sleep(1s)) {
        break;  // Stop requested during pause
      }
    }
  }

  // ──────────────────────────────────────────────────────────
  // Shutdown and final summary
  // ──────────────────────────────────────────────────────────

  mgr.shutdown();

  // Return arms to staging and then sleep position (if using hardware)
  if (!cfg.use_mock && leader_driver && follower_driver) {
    std::cout << "\nReturning arms to starting positions...\n";
    leader_driver->set_all_modes(trossen_arm::Mode::position);
    leader_driver->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
    follower_driver->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
    leader_driver->set_all_positions(
      std::vector<double>(leader_driver->get_num_joints(), 0.0),
      moving_time_s,
      false);
    follower_driver->set_all_positions(
      std::vector<double>(follower_driver->get_num_joints(), 0.0),
      moving_time_s,
      false);
    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
    std::cout << "Arms returned to sleep position.\n";
  }

  auto final_stats = mgr.stats();
  print_final_summary(final_stats.total_episodes_completed, cfg.output_dir);

  return 0;
}
