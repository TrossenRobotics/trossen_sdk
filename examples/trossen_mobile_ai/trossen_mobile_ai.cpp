/**
 * @file trossen_mobile_ai.cpp
 * @brief Bimanual AI Kit demo - 2 leader + 2 follower arms + SLATE mobile base + 3 cameras
 *
 * Records two leader/follower arm pairs, SLATE mobile base velocity data, and
 * cameras (ZED, RealSense, or OpenCV). All hardware parameters, session settings,
 * and teleop setup are driven by a single JSON config file with optional CLI overrides.
 *
 * Usage:
 *   ./trossen_mobile_ai [OPTIONS]
 *
 *   --config PATH       Path to robot config JSON
 *                       [default: examples/trossen_mobile_ai/config.json]
 *   --set KEY=VALUE     Override a config value using dot notation (repeatable)
 *   --dump-config       Print merged config as JSON and exit
 *   --help              Show this help and exit
 *
 * Examples:
 *   ./trossen_mobile_ai
 *   ./trossen_mobile_ai --config examples/trossen_mobile_ai/config.json
 *   ./trossen_mobile_ai --set hardware.arms.leader_left.ip_address=192.168.1.3
 *   ./trossen_mobile_ai --set session.max_duration=30 --set session.max_episodes=10
 *   ./trossen_mobile_ai --dump-config
 */

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"
#include "nlohmann/json.hpp"
#include "trossen_sdk/configuration/cli_parser.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/configuration/sdk_config.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/arm/trossen_arm_producer.hpp"
#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/base/slate_base_component.hpp"
#include "trossen_sdk/hw/base/slate_base_producer.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/teleop/teleop_factory.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"
#include "trossen_sdk/runtime/push_producer_registry.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"

#include "trossen_sdk/utils/app_utils.hpp"

static void print_usage(const char* program) {
  std::cout <<
    "Usage: " << program << " [OPTIONS]\n"
    "\n"
    "Options:\n"
    "  --config PATH      Path to robot config JSON\n"
    "                     [default: examples/trossen_mobile_ai/config.json]\n"
    "  --set KEY=VALUE    Override a config value using dot notation (repeatable)\n"
    "  --dump-config      Print merged config as JSON and exit\n"
    "  --help             Show this help and exit\n"
    "\n"
    "Examples:\n"
    "  " << program << "\n"
    "  " << program << " --config examples/trossen_mobile_ai/config.json\n"
    "  " << program << " --set hardware.arms.leader_left.ip_address=192.168.1.3\n"
    "  " << program << " --set session.max_duration=30\n"
    "  " << program << " --dump-config\n";
}

int main(int argc, char** argv) {
  trossen::configuration::CliParser cli(argc, argv);

  if (cli.has_flag("help")) {
    print_usage(argv[0]);
    return 0;
  }

  const std::string config_path =
    cli.get_string("config", "examples/trossen_mobile_ai/config.json");

  if (!std::filesystem::exists(config_path)) {
    std::cerr << "Error: config file not found: " << config_path << "\n";
    return 1;
  }

  // Load JSON and apply CLI overrides
  auto j = trossen::configuration::JsonLoader::load(config_path);
  const auto overrides = cli.get_set_overrides();
  if (!overrides.empty()) {
    j = trossen::configuration::merge_overrides(j, overrides);
  }

  if (cli.has_flag("dump-config")) {
    trossen::configuration::dump_config(j, "Trossen Mobile AI Kit Config");
    return 0;
  }

  // Parse unified robot config
  auto cfg = trossen::configuration::SdkConfig::from_json(j);

  // Populate GlobalConfig so SessionManager picks up session + backend settings
  cfg.populate_global_config();

  // Derive data root path from the TrossenMCAP backend config
  const std::string root = cfg.mcap_backend.root;

  // Derive per-type rates from the producer list
  float joint_rate_hz = 30.0f;
  float camera_fps = 30.0f;
  float base_rate_hz = 30.0f;
  for (const auto& p : cfg.producers) {
    if (p.type == "trossen_arm") {
      joint_rate_hz = p.poll_rate_hz;
      break;
    }
  }
  for (const auto& p : cfg.producers) {
    if (p.type == "zed_camera" || p.type == "realsense_camera" ||
        p.type == "opencv_camera") {
      camera_fps = p.poll_rate_hz;
      break;
    }
  }
  for (const auto& p : cfg.producers) {
    if (p.type == "slate_base") {
      base_rate_hz = p.poll_rate_hz;
      break;
    }
  }

  std::vector<std::string> config_lines = {
    "Config file:          " + config_path,
    "Root directory:       " + root,
    "Backend:              " + cfg.session.backend_type,
    "Robot name:           " + cfg.robot_name
  };
  for (const auto& [id, arm] : cfg.hardware.arms) {
    config_lines.push_back(
      "Arm [" + id + "]:  " + arm.ip_address + " (" + arm.end_effector + ")");
  }
  config_lines.push_back("Joint rate:           " + std::to_string(joint_rate_hz) + " Hz");
  if (cfg.hardware.mobile_base.has_value()) {
    config_lines.push_back("Mobile base rate:     " + std::to_string(base_rate_hz) + " Hz");
  }
  for (const auto& cam : cfg.hardware.cameras) {
    config_lines.push_back(
      "Camera [" + cam.id + "]:  " + cam.serial_number + "  " +
      std::to_string(cam.width) + "x" + std::to_string(cam.height) +
      " @ " + std::to_string(cam.fps) + " fps");
  }
  config_lines.push_back(
    "Teleop:               " +
    std::string(cfg.teleop.enabled ? "enabled" : "disabled") +
    " (" + std::to_string(cfg.teleop.pairs.size()) + " pairs)");

  trossen::utils::print_config_banner("Trossen Mobile AI Kit Demo Usage", config_lines);

  trossen::utils::install_signal_handler();
  std::filesystem::create_directories(root);

  // ──────────────────────────────────────────────────────────
  // Initialize arm hardware from config
  // ──────────────────────────────────────────────────────────

  std::cout << "Initializing hardware...\n";

  std::unordered_map<std::string,
    std::shared_ptr<trossen::hw::arm::TrossenArmComponent>> arm_components;

  for (const auto& [id, arm_cfg] : cfg.hardware.arms) {
    auto component = trossen::hw::HardwareRegistry::create(
      "trossen_arm", id, arm_cfg.to_json(), true);
    arm_components[id] =
      std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(component);
    std::cout << "  [ok] Arm [" << id << "] configured (" << arm_cfg.ip_address << ")\n";
  }

  // Initialize SLATE mobile base (if configured)
  std::shared_ptr<trossen::hw::HardwareComponent> slate_base_component;
  if (cfg.hardware.mobile_base.has_value()) {
    slate_base_component =
      std::make_shared<trossen::hw::base::SlateBaseComponent>("slate_base");
    slate_base_component->configure(cfg.hardware.mobile_base->to_json());
    trossen::hw::ActiveHardwareRegistry::register_active("slate_base", slate_base_component);
    std::cout << "  [ok] SLATE mobile base configured\n";
  }

  // ──────────────────────────────────────────────────────────
  // Session Manager + producers
  // ──────────────────────────────────────────────────────────

  trossen::runtime::SessionManager mgr;
  std::cout << "\nInitialized Session Manager\n";
  std::cout << "  Starting episode index: " << mgr.stats().current_episode_index << "\n";
  if (mgr.stats().current_episode_index > 0) {
    std::cout << "  (Resuming from existing episodes in directory)\n";
  }
  std::cout << "\n";

  // Pre-initialize camera hardware (keyed by camera id for producer lookup)
  std::unordered_map<std::string,
    std::shared_ptr<trossen::hw::HardwareComponent>> camera_components;
  std::unordered_map<std::string,
    const trossen::configuration::CameraConfig*> camera_cfg_map;
  for (const auto& cam_cfg : cfg.hardware.cameras) {
    auto cam_component = trossen::hw::HardwareRegistry::create(
      cam_cfg.type, cam_cfg.id, cam_cfg.to_json());
    camera_components[cam_cfg.id] = cam_component;
    camera_cfg_map[cam_cfg.id] = &cam_cfg;
    std::cout << "  [ok] Camera [" << cam_cfg.id << "] initialized ("
              << cam_cfg.serial_number << ")\n";
  }

  std::cout << "Creating producers...\n";

  // base_prod must outlive the loop - used later in the velocity thread
  std::shared_ptr<trossen::hw::PolledProducer> base_prod;

  // One producer per entry in the producers list
  for (const auto& prod_cfg : cfg.producers) {
    const auto period =
      std::chrono::milliseconds(static_cast<int>(1000.0f / prod_cfg.poll_rate_hz));

    if (prod_cfg.type == "trossen_arm") {
      auto prod = trossen::runtime::ProducerRegistry::create(
        "trossen_arm",
        arm_components.at(prod_cfg.hardware_id),
        prod_cfg.to_registry_json());
      mgr.add_producer(prod, period);
      std::cout << "  [ok] Arm producer [" << prod_cfg.stream_id << "] ("
                << prod_cfg.poll_rate_hz << " Hz)\n";
    } else if (prod_cfg.type == "slate_base" && slate_base_component) {
      base_prod = trossen::runtime::ProducerRegistry::create(
        "slate_base",
        slate_base_component,
        prod_cfg.to_registry_json());
      mgr.add_producer(base_prod, period);
      std::cout << "  [ok] Mobile base producer (" << prod_cfg.poll_rate_hz << " Hz)\n";
    } else if (camera_components.count(prod_cfg.hardware_id)) {
      const auto* cam = camera_cfg_map.at(prod_cfg.hardware_id);
      if (trossen::runtime::PushProducerRegistry::is_registered(prod_cfg.type)) {
        auto prod = trossen::runtime::PushProducerRegistry::create(
          prod_cfg.type,
          camera_components.at(prod_cfg.hardware_id),
          prod_cfg.to_registry_json(cam->width, cam->height, cam->fps));
        mgr.add_push_producer(prod);
        std::cout << "  [ok] Camera producer (push) [" << prod_cfg.stream_id << "] ("
                  << cam->width << "x" << cam->height << ")\n";
      } else {
        auto prod = trossen::runtime::ProducerRegistry::create(
          prod_cfg.type,
          camera_components.at(prod_cfg.hardware_id),
          prod_cfg.to_registry_json(cam->width, cam->height, cam->fps));
        mgr.add_producer(prod, period);
        std::cout << "  [ok] Camera producer [" << prod_cfg.stream_id << "] ("
                  << prod_cfg.poll_rate_hz << " Hz, "
                  << cam->width << "x" << cam->height << ")\n";
      }
    }
  }

  std::cout << "\nProducers registered. Ready to record.\n";

  // ──────────────────────────────────────────────────────────
  // Teleop controllers (constructor stages arms)
  // ──────────────────────────────────────────────────────────

  auto controllers = trossen::hw::teleop::create_controllers_from_global_config();

  // ──────────────────────────────────────────────────────────
  // Register lifecycle callbacks
  // ──────────────────────────────────────────────────────────

  // Before each episode: let controllers run their pre-episode lifecycle
  mgr.on_pre_episode([&]() -> bool {
    for (auto& ctrl : controllers) ctrl->prepare_teleop();
    return true;
  });

  // Episode started: begin mirroring (alongside recording)
  mgr.on_episode_started([&]() {
    for (auto& ctrl : controllers) ctrl->teleop();
    std::cout << "Episode started - recording and mirroring active.\n";
  });

  // Count depth-capable cameras once for sanity checks
  int depth_cameras = 0;
  for (const auto& cam : cfg.hardware.cameras) {
    if (cam.use_depth) ++depth_cameras;
  }

  // After each episode: reset mode (mirroring continues, no recording)
  mgr.on_episode_ended([&](const trossen::runtime::SessionManager::Stats& stats) {
    for (auto& ctrl : controllers) ctrl->reset_teleop();

    const std::string file_path =
      trossen::utils::generate_episode_path(root, stats.current_episode_index);
    trossen::utils::print_episode_summary(file_path, stats);

    trossen::utils::SanityCheckConfig sanity_cfg{
      stats.elapsed.count(),
      static_cast<int>(cfg.hardware.arms.size()),
      joint_rate_hz,
      static_cast<int>(cfg.hardware.cameras.size()),
      static_cast<int>(camera_fps),
      5.0,
      depth_cameras
    };
    perform_sanity_check(stats.current_episode_index, stats.records_written_current, sanity_cfg);
  });

  // Shutdown: stop mirror + return arms to rest
  mgr.on_pre_shutdown([&]() {
    for (auto& ctrl : controllers) ctrl->stop_teleop();
  });

  // ──────────────────────────────────────────────────────────
  // Episode loop
  // ──────────────────────────────────────────────────────────

  while (true) {
    if (trossen::utils::g_stop_requested) {
      std::cout << "\n\nStopping at user request (Ctrl+C).\n";
      break;
    }

    mgr.print_episode_header();

    if (!mgr.start_episode()) {
      break;
    }

    std::cout << "Recording...\n";

    // Velocity polling thread keeps base_prod active during the episode
    std::thread velocity_thread([&]() {
      if (!base_prod) {
        return;
      }
      while (mgr.is_episode_active() && !trossen::utils::g_stop_requested) {
        base_prod->poll([](std::shared_ptr<trossen::data::RecordBase>) {});
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });

    auto action = mgr.monitor_episode();

    if (action == trossen::runtime::UserAction::kReRecord) {
      mgr.discard_current_episode();
      if (velocity_thread.joinable()) {
        velocity_thread.join();
      }
      continue;
    }

    if (mgr.is_episode_active()) {
      mgr.stop_episode();
    }
    if (velocity_thread.joinable()) {
      velocity_thread.join();
    }

    if (trossen::utils::g_stop_requested) {
      std::cout << "\nStopping at user request (Ctrl+C).\n";
      break;
    }

    action = mgr.wait_for_reset();
    if (action == trossen::runtime::UserAction::kStop) break;
    if (action == trossen::runtime::UserAction::kReRecord) {
      mgr.discard_last_episode();
      continue;
    }
  }

  // shutdown() calls stop_episode() (no-op if already stopped) then on_pre_shutdown
  mgr.shutdown();

  const auto final_stats = mgr.stats();
  std::vector<std::string> extra_info = {
    "Data streams:         " +
      std::to_string(cfg.hardware.arms.size()) + " arms" +
      (slate_base_component ? " + mobile base velocity" : "") +
      " + " + std::to_string(cfg.hardware.cameras.size()) + " cameras"
  };
  trossen::utils::print_final_summary(
    final_stats.total_episodes_completed, root, extra_info);

  return 0;
}
