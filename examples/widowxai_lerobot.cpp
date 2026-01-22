/**
 * @file widowxai_lerobot.cpp
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
 *   ./widowxai_lerobot
 *   ./widowxai_lerobot --root-dir /data/recordings
 *   ./widowxai_lerobot --mock  # Use mock producers for testing without hardware
 */

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"
#include "trossen_sdk/hw/arm/teleop_mock_joint_producer.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"
#include "trossen_sdk/hw/arm/teleop_arm_producer.hpp"
#include "trossen_sdk/hw/arm/teleop_mock_joint_producer.hpp"
#include "trossen_sdk/hw/camera/opencv_producer.hpp"
#include "trossen_sdk/hw/camera/mock_producer.hpp"

#ifdef TROSSEN_ENABLE_ZED
#include "trossen_sdk/hw/camera/zed_camera_component.hpp"
#include "trossen_sdk/hw/camera/zed_depth_producer.hpp"
#include "trossen_sdk/hw/camera/zed_frame_cache.hpp"
#include "trossen_sdk/hw/camera/zed_producer.hpp"
#endif

#ifdef TROSSEN_ENABLE_REALSENSE
#include "trossen_sdk/hw/camera/realsense_depth_producer.hpp"
#include "trossen_sdk/hw/camera/realsense_frame_cache.hpp"
#include "trossen_sdk/hw/camera/realsense_producer.hpp"
#endif

#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"

#include "./demo_utils.hpp"


// ────────────────────────────────────────────────────────────
// Command-line configuration
// ────────────────────────────────────────────────────────────

struct Config {
  std::string dataset_id = "";  // empty = auto-generate
  std::string root = trossen::io::backends::get_default_root_path().string();
  std::string repository_id = "TrossenRoboticsCommunity";  // Valid only for LeRobot backend
  bool use_mock = false;
  bool show_help = false;

  // Camera settings
  std::string camera_type = "zed";  // mock, opencv, realsense, zed
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
  std::cout
    << "Usage: " << prog_name << " [options]\n\n"
    << "Options:\n"
    << "  --dataset-id <string>    Dataset identifier (default: auto-generate UUID)\n"
    << "  --root <path>            Root directory for episodes (default: ~/.cache/trossen_sdk/)\n"
    << "  --repository-id <string> Repository identifier (default: TrossenRoboticsCommunity, "
    << "only for LeRobot backend)\n"
    << "  --mock                   Use mock producers instead of real hardware\n"
    << "  --camera <type>          Camera type: mock, opencv, realsense, zed (default: zed)\n"
    << "  --camera-index <num>     Camera device index for opencv (default: 2, i.e., /dev/video2)\n"
    << "  --camera-width <pixels>  Camera width (default: 1920)\n"
    << "  --camera-height <pixels> Camera height (default: 1080)\n"
    << "  --camera-fps <fps>       Camera frame rate (default: 30)\n"
    << "  --joint-rate <hz>        Joint state capture rate (default: 30)\n"
    << "  --leader-ip <ip>         Leader arm IP (default: 192.168.1.2)\n"
    << "  --follower-ip <ip>       Follower arm IP (default: 192.168.1.4)\n"
    << "  --backend <type>         Dataset backend type (default: mcap)\n"
    << "  --help                   Show this help message\n\n"
    << "Examples:\n"
    << "  " << prog_name << "\n"
    << "  " << prog_name << " --mock\n"
    << "  " << prog_name << " --camera realsense\n"
    << "  " << prog_name << " --camera opencv --camera-index 0\n"
    << "  " << prog_name << " --dataset-id solo_demo_001 --root /data/recordings\n";
}

Config parse_args(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      cfg.show_help = true;
      return cfg;
    } else if (arg == "--dataset-id" && i + 1 < argc) {
      cfg.dataset_id = argv[++i];
    } else if (arg == "--root" && i + 1 < argc) {
      cfg.root = argv[++i];
    } else if (arg == "--repository-id" && i + 1 < argc) {
      cfg.repository_id = argv[++i];
    } else if (arg == "--mock") {
      cfg.use_mock = true;
    } else if (arg == "--camera" && i + 1 < argc) {
      cfg.camera_type = argv[++i];
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
  if (!std::filesystem::exists("config/sdk_config.json")) {
    std::cerr << "Error: config/sdk_config.json not found!" << std::endl;
    return 1;
  }

  // Create and load global configuration
  auto j = trossen::configuration::JsonLoader::load("config/sdk_config.json");
  trossen::configuration::GlobalConfig::instance().load_from_json(j);

  // Note: --mock flag only affects robot hardware, not camera
  // Use --camera mock to explicitly use mock camera

  // Print configuration
  std::vector<std::string> config_lines = {
    "Mode:                 " + std::string(cfg.use_mock ? "Mock (no hardware)" : "Hardware"),
    "Dataset ID:           " + (cfg.dataset_id.empty() ? "<auto-generate>" : cfg.dataset_id),
    "Root directory:       " + cfg.root,
    "Repository ID:        " + cfg.repository_id,
    "Backend:              " + cfg.backend_type,
    "Camera Type:          " + cfg.camera_type
  };

  if (!cfg.use_mock) {
    config_lines.push_back("Leader IP:            " + cfg.leader_ip);
    config_lines.push_back("Follower IP:          " + cfg.follower_ip);
  }

  config_lines.push_back("Joint rate:           " + std::to_string(cfg.joint_rate_hz) + " Hz");

  // Add camera-specific info
  if (cfg.camera_type == "opencv") {
    config_lines.push_back(
      "Camera:               /dev/video" + std::to_string(cfg.camera_index) +
      " @ " + std::to_string(cfg.camera_width) + "x" + std::to_string(cfg.camera_height) +
      " @ " + std::to_string(cfg.camera_fps) + " fps");
  } else if (cfg.camera_type == "mock") {
    config_lines.push_back(
      "Camera:               Mock (" +
      std::to_string(cfg.camera_width) + "x" + std::to_string(cfg.camera_height) +
      " @ " + std::to_string(cfg.camera_fps) + " fps)");
  } else {
    // For ZED and RealSense, just show the type (details will be shown during setup)
    config_lines.push_back("Camera:               " + cfg.camera_type);
  }

  trossen::demo::print_config_banner("Trossen AI LeRobot Solo Complete Demo", config_lines);

  // Install signal handler for graceful shutdown
  trossen::demo::install_signal_handler();

  // Create root directory
  std::filesystem::create_directories(cfg.root);

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
    follower_driver->set_joint_limits(joint_limits);

    // Move arms to staged positions
    leader_driver->set_all_modes(trossen_arm::Mode::position);
    follower_driver->set_all_modes(trossen_arm::Mode::position);
    leader_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);
    follower_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);
    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));

    std::cout << "  ✓ Arms staged to starting positions\n";
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

  // Camera producer setup

  std::cout << "\nSetting up camera producer (type: " << cfg.camera_type << ")...\n";

  std::shared_ptr<trossen::hw::PolledProducer> camera_producer;
  std::shared_ptr<trossen::hw::PolledProducer> depth_producer;  // Optional depth producer
  auto camera_period = std::chrono::milliseconds(static_cast<int>(1000.0f / cfg.camera_fps));

  // Component storage (must outlive producers)
#ifdef TROSSEN_ENABLE_ZED
  std::shared_ptr<trossen::hw::camera::ZedCameraComponent> zed_component;
#endif
#ifdef TROSSEN_ENABLE_REALSENSE
  std::shared_ptr<rs2::pipeline> rs_pipeline_ptr;
#endif

  if (cfg.camera_type == "mock") {
    // Mock Camera
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

  } else if (cfg.camera_type == "opencv") {
    // OpenCV Camera
    trossen::hw::camera::OpenCvCameraProducer::Config cam_cfg;
    cam_cfg.device_index = cfg.camera_index;
    cam_cfg.stream_id = "camera_main";
    cam_cfg.encoding = "bgr8";
    cam_cfg.width = cfg.camera_width;
    cam_cfg.height = cfg.camera_height;
    cam_cfg.fps = cfg.camera_fps;
    cam_cfg.use_device_time = false;
    camera_producer = std::make_shared<trossen::hw::camera::OpenCvCameraProducer>(cam_cfg);
    std::cout << "  ✓ OpenCV camera producer (" << cfg.camera_fps << " Hz, "
              << cfg.camera_width << "x" << cfg.camera_height << ", /dev/video"
              << cfg.camera_index << ")\n";

#ifdef TROSSEN_ENABLE_REALSENSE
  } else if (cfg.camera_type == "realsense") {
    // RealSense Camera
    // Note: This section can be customized with command-line args if needed
    std::string serial_number = "";  // Empty = use any available camera
    int rs_width = 640;
    int rs_height = 480;
    int rs_fps = 30;
    bool enable_depth = true;

    // Create RealSense config
    rs2::config cam_cfg;
    rs2::pipeline rs_pipeline;
    rs_pipeline_ptr = std::make_shared<rs2::pipeline>(rs_pipeline);

    // Enable the device using serial number (if provided)
    if (!serial_number.empty()) {
      cam_cfg.enable_device(serial_number);
    }

    // Enable color and depth streams
    cam_cfg.enable_stream(RS2_STREAM_COLOR, rs_width, rs_height, RS2_FORMAT_RGB8, rs_fps);
    if (enable_depth) {
      cam_cfg.enable_stream(RS2_STREAM_DEPTH, rs_width, rs_height, RS2_FORMAT_Z16, rs_fps);
    }

    // Start the camera pipeline
    try {
      rs2::pipeline_profile profile = rs_pipeline_ptr->start(cam_cfg);
      auto dev = profile.get_device();
      serial_number = std::string(dev.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER));
      std::cout << "  ✓ RealSense camera opened (SN: " << serial_number << ")\n";
    } catch (const rs2::error& e) {
      std::cerr << "Failed to start RealSense camera: " << e.what() << "\n";
      return 1;
    }

    // Create frame cache (consumer count = 2 if depth enabled, else 1)
    int consumer_count = enable_depth ? 2 : 1;
    auto frame_cache =
      std::make_shared<trossen::hw::camera::RealsenseFrameCache>(rs_pipeline_ptr, consumer_count);

    // Create color producer
    trossen::hw::camera::RealsenseCameraProducer::Config rs_cfg;
    rs_cfg.serial_number = serial_number;
    rs_cfg.stream_id = "realsense_camera_main";
    rs_cfg.encoding = "bgr8";
    rs_cfg.width = rs_width;
    rs_cfg.height = rs_height;
    rs_cfg.fps = rs_fps;
    rs_cfg.use_device_time = true;
    rs_cfg.warmup_seconds = 2.0;

    camera_producer =
      std::make_shared<trossen::hw::camera::RealsenseCameraProducer>(frame_cache, rs_cfg);
    std::cout << "  ✓ RealSense color producer (" << rs_fps << " Hz, "
              << rs_width << "x" << rs_height << ")\n";

    // Create depth producer (if enabled)
    if (enable_depth) {
      trossen::hw::camera::RealsenseDepthCameraProducer::Config rs_depth_cfg;
      rs_depth_cfg.serial_number = serial_number;
      rs_depth_cfg.stream_id = "realsense_depth_camera_main";
      rs_depth_cfg.encoding = "16UC1";
      rs_depth_cfg.width = rs_width;
      rs_depth_cfg.height = rs_height;
      rs_depth_cfg.fps = rs_fps;
      rs_depth_cfg.use_device_time = true;
      rs_depth_cfg.warmup_seconds = 2.0;

      depth_producer =
        std::make_shared<trossen::hw::camera::RealsenseDepthCameraProducer>(
          frame_cache, rs_depth_cfg);
      std::cout << "  ✓ RealSense depth producer (" << rs_fps << " Hz, "
                << rs_width << "x" << rs_height << ")\n";
    }
#endif

#ifdef TROSSEN_ENABLE_ZED
  } else if (cfg.camera_type == "zed") {
    // ────────────── ZED Camera ──────────────
    // Note: This section can be customized with command-line args if needed
    std::string resolution = "SVGA";  // SVGA, HD720, HD1080, HD2K
    int zed_fps = 30;
    bool enable_depth = true;
    std::string depth_mode = "PERFORMANCE";  // PERFORMANCE, QUALITY, ULTRA, NEURAL

    // Create ZED camera component (must outlive producers)
    zed_component = std::make_shared<trossen::hw::camera::ZedCameraComponent>("zed_main");

    nlohmann::json zed_config = {
      {"resolution", resolution},
      {"fps", zed_fps},
      {"use_depth", enable_depth},
      {"depth_mode", depth_mode}
    };

    zed_component->configure(zed_config);
    auto zed_cache = zed_component->get_hardware();
    int serial_number = zed_component->get_serial_number();

    // Create ZED color producer
    trossen::hw::camera::ZedCameraProducer::Config zed_cfg;
    zed_cfg.serial_number = serial_number;
    zed_cfg.stream_id = "zed_camera_main";
    zed_cfg.encoding = "bgr8";
    zed_cfg.width = 0;   // 0 = use camera default resolution
    zed_cfg.height = 0;
    zed_cfg.fps = zed_fps;
    zed_cfg.use_device_time = true;
    zed_cfg.warmup_seconds = 1.0;

    camera_producer = std::make_shared<trossen::hw::camera::ZedCameraProducer>(zed_cache, zed_cfg);

    // Perform warmup
    auto zed_producer =
      std::dynamic_pointer_cast<trossen::hw::camera::ZedCameraProducer>(camera_producer);
    if (zed_producer) {
      zed_producer->warmup();
    }

    std::cout << "  ✓ ZED color producer (" << zed_fps << " Hz, " << resolution
              << ", SN: " << serial_number << ")\n";

    // Create ZED depth producer (if enabled)
    if (enable_depth) {
      trossen::hw::camera::ZedDepthCameraProducer::Config zed_depth_cfg;
      zed_depth_cfg.serial_number = serial_number;
      zed_depth_cfg.stream_id = "zed_depth_camera_main";
      zed_depth_cfg.encoding = "16UC1";
      zed_depth_cfg.width = 0;  // 0 = use camera default resolution
      zed_depth_cfg.height = 0;
      zed_depth_cfg.fps = zed_fps;
      zed_depth_cfg.use_device_time = true;
      zed_depth_cfg.warmup_seconds = 1.0;

      depth_producer =
        std::make_shared<trossen::hw::camera::ZedDepthCameraProducer>(zed_cache, zed_depth_cfg);

      // Perform warmup for depth
      if (depth_producer) {
        auto zed_depth =
          std::dynamic_pointer_cast<trossen::hw::camera::ZedDepthCameraProducer>(depth_producer);
        if (zed_depth) {
          zed_depth->warmup();
        }
      }

      std::cout << "  ✓ ZED depth producer (" << zed_fps << " Hz, " << resolution
                << ", mode: " << depth_mode << ")\n";
    }
#endif

  } else {
    std::cerr << "Error: Unknown camera type '" << cfg.camera_type << "'\n";
    std::cerr << "Supported types: mock, opencv";
#ifdef TROSSEN_ENABLE_REALSENSE
    std::cerr << ", realsense";
#endif
#ifdef TROSSEN_ENABLE_ZED
    std::cerr << ", zed";
#endif
    std::cerr << "\n";
    return 1;
  }

  // Register camera producer(s)
  mgr.add_producer(camera_producer, camera_period);
  if (depth_producer) {
    mgr.add_producer(depth_producer, camera_period);
  }

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

  // TODO(shantanuparab-tr): Create a method for checking if all the hardware
  // producers are ready before starting the recording. For now, adding a sleep.
  // Sleep briefly to ensure everything is settled
  std::this_thread::sleep_for(std::chrono::seconds(5));

  // ──────────────────────────────────────────────────────────
  // Recording loop: record requested number of episodes
  // ──────────────────────────────────────────────────────────

  while (true) {
    if (trossen::demo::g_stop_requested) {
      std::cout << "\n\nStopping at user request (Ctrl+C).\n";
      break;
    }

    // Display episode header
    mgr.print_episode_header();

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
        while (mgr.is_episode_active() && !trossen::demo::g_stop_requested) {
          auto leader_js = leader_driver->get_all_positions();
          follower_driver->set_all_positions(leader_js, 0.0f, false);
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

    // ──────────────────────────────────────────────────────────
    // Sanity check: verify expected record counts
    // ──────────────────────────────────────────────────────────

    // Calculate expected camera count (1 or 2 depending on depth)
    int camera_count = depth_producer ? 2 : 1;

    trossen::demo::SanityCheckConfig sanity_cfg{
      last_stats.recording_duration_s.value_or(0.0),
      1,  // 1 joint producer (follower)
      cfg.joint_rate_hz,
      camera_count,
      cfg.camera_fps,
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

  // ──────────────────────────────────────────────────────────
  // Shutdown and final summary
  // ──────────────────────────────────────────────────────────

  mgr.shutdown();

  // Return arms to staging and then sleep position (if using hardware)
  if (!cfg.use_mock && leader_driver && follower_driver) {
    std::cout << "\nReturning arms to starting positions...\n";
    leader_driver->set_all_modes(trossen_arm::Mode::position);
    leader_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);
    follower_driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);
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
  trossen::demo::print_final_summary(final_stats.total_episodes_completed, cfg.root);

  return 0;
}
