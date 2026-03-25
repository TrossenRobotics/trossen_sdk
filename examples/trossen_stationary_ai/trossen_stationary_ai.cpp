/**
 * @file trossen_stationary_ai.cpp
 * @brief Bimanual AI Kit demo - 4 arms + RealSense cameras with config-driven setup
 *
 * Records all 4 arms (both leaders and followers) along with RealSense cameras.
 * All hardware parameters, session settings, and teleop setup are driven by a
 * single JSON config file with optional CLI overrides.
 *
 * Usage:
 *   ./trossen_stationary_ai [OPTIONS]
 *
 *   --config PATH       Path to robot config JSON
 *                       [default: examples/trossen_stationary_ai/config.json]
 *   --set KEY=VALUE     Override a config value using dot notation (repeatable)
 *   --dump-config       Print merged config as JSON and exit
 *   --help              Show this help and exit
 *
 * Examples:
 *   ./trossen_stationary_ai
 *   ./trossen_stationary_ai --config examples/trossen_stationary_ai/config.json
 *   ./trossen_stationary_ai --set hardware.arms.leader_left.ip_address=192.168.1.3
 *   ./trossen_stationary_ai --set session.max_duration=30 --set session.backend_type=lerobot
 *   ./trossen_stationary_ai --dump-config
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
#include "trossen_sdk/hw/arm/trossen_arm_producer.hpp"
#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"
#include "trossen_sdk/runtime/push_producer_registry.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"

#include "trossen_sdk/utils/app_utils.hpp"

static const std::vector<double> STAGED_POSITIONS = {
  0.0, 1.04719755, 0.523598776, 0.628318531, 0.0, 0.0, 0.0
};

static void print_usage(const char* program) {
  std::cout <<
    "Usage: " << program << " [OPTIONS]\n"
    "\n"
    "Options:\n"
    "  --config PATH      Path to robot config JSON\n"
    "                     [default: examples/trossen_stationary_ai/config.json]\n"
    "  --set KEY=VALUE    Override a config value using dot notation (repeatable)\n"
    "  --dump-config      Print merged config as JSON and exit\n"
    "  --help             Show this help and exit\n"
    "\n"
    "Examples:\n"
    "  " << program << "\n"
    "  " << program << " --config examples/trossen_stationary_ai/config.json\n"
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
    cli.get_string("config", "examples/trossen_stationary_ai/config.json");

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
    trossen::configuration::dump_config(j, "Trossen Stationary AI Kit Config");
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
  for (const auto& p : cfg.producers) {
    if (p.type == "trossen_arm") {
      joint_rate_hz = p.poll_rate_hz;
      break;
    }
  }
  for (const auto& p : cfg.producers) {
    if (p.type == "realsense_camera" || p.type == "opencv_camera" ||
        p.type == "zed_camera") {
      camera_fps = p.poll_rate_hz;
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

  trossen::utils::print_config_banner("Trossen Stationary AI Kit Demo Usage", config_lines);

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

  // Stage all arms to starting positions
  const float moving_time_s = 2.0f;
  for (const auto& [id, comp] : arm_components) {
    auto driver = comp->get_hardware();
    driver->set_all_modes(trossen_arm::Mode::position);
    driver->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
  }
  std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
  std::cout << "  [ok] All arms staged to starting positions\n";

  // Adjust gripper tolerance for follower arms
  for (const auto& pair : cfg.teleop.pairs) {
    auto it = arm_components.find(pair.follower);
    if (it == arm_components.end()) {
      continue;
    }
    auto driver = it->second->get_hardware();
    auto limits = driver->get_joint_limits();
    limits[driver->get_num_joints() - 1].position_tolerance = 0.01;
    driver->set_joint_limits(limits);
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
  // Register lifecycle callbacks
  // ──────────────────────────────────────────────────────────

  // Count depth-capable cameras once for sanity checks
  int depth_cameras = 0;
  for (const auto& cam : cfg.hardware.cameras) {
    if (cam.use_depth) ++depth_cameras;
  }

  mgr.on_episode_ended([&](const trossen::runtime::SessionManager::Stats& stats) {
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

  mgr.on_pre_shutdown([&]() {
    std::cout << "\nReturning arms to starting positions...\n";
    for (const auto& [id, comp] : arm_components) {
      auto driver = comp->get_hardware();
      driver->set_all_modes(trossen_arm::Mode::position);
      driver->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
    }
    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));

    std::cout << "Moving arms to sleep positions...\n";
    for (const auto& [id, comp] : arm_components) {
      auto driver = comp->get_hardware();
      driver->set_all_positions(
        std::vector<double>(driver->get_num_joints(), 0.0), moving_time_s, false);
    }
    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));

    std::cout << "Cleaning up arm drivers...\n";
    for (const auto& [id, comp] : arm_components) {
      comp->get_hardware()->cleanup();
    }
    std::cout << "  [ok] All arm drivers cleaned up\n";
  });

  // ──────────────────────────────────────────────────────────
  // Episode loop
  // ──────────────────────────────────────────────────────────

  while (true) {
    if (trossen::utils::g_stop_requested) {
      std::cout << "\n\nStopping at user request (Ctrl+C).\n";
      break;
    }

    if (cfg.teleop.enabled) {
      std::cout << "\nLocking leader arms and moving followers to match...\n";
      for (const auto& pair : cfg.teleop.pairs) {
        auto leader_it = arm_components.find(pair.leader);
        auto follower_it = arm_components.find(pair.follower);
        if (leader_it == arm_components.end() || follower_it == arm_components.end()) {
          continue;
        }
        auto leader_driver = leader_it->second->get_hardware();
        auto follower_driver = follower_it->second->get_hardware();
        leader_driver->set_all_modes(trossen_arm::Mode::position);
        follower_driver->set_all_modes(trossen_arm::Mode::position);
        follower_driver->set_all_positions(
          leader_driver->get_all_positions(), moving_time_s, false);
      }
      std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
      std::cout << "  [ok] Followers moved to match leaders\n";

      std::cout << "\n!! Enabling teleop mode !!\n";
      for (const auto& pair : cfg.teleop.pairs) {
        auto it = arm_components.find(pair.leader);
        if (it == arm_components.end()) {
          continue;
        }
        auto driver = it->second->get_hardware();
        driver->set_all_modes(trossen_arm::Mode::external_effort);
        driver->set_all_external_efforts(
          std::vector<double>(driver->get_num_joints(), 0.0), 0.0, false);
        std::cout << "   " << pair.leader << ": gravity compensation -> "
                  << pair.follower << " will mirror\n";
      }
      std::cout << "\n";
    }

    mgr.print_episode_header();

    if (!mgr.start_episode()) {
      break;
    }

    std::cout << "Recording...\n";

    // Teleop thread mirrors leader positions to followers at configured rate
    std::thread teleop_thread([&]() {
      if (!cfg.teleop.enabled) {
        return;
      }
      const auto sleep_ns = static_cast<int64_t>(1e9 / cfg.teleop.rate_hz);
      while (mgr.is_episode_active() && !trossen::utils::g_stop_requested) {
        for (const auto& pair : cfg.teleop.pairs) {
          auto leader_it = arm_components.find(pair.leader);
          auto follower_it = arm_components.find(pair.follower);
          if (leader_it == arm_components.end() || follower_it == arm_components.end()) {
            continue;
          }
          follower_it->second->get_hardware()->set_all_positions(
            leader_it->second->get_hardware()->get_all_positions(), 0.0f, false);
        }
        std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
      }
    });

    mgr.monitor_episode();

    if (mgr.is_episode_active()) {
      mgr.stop_episode();
    }
    if (teleop_thread.joinable()) {
      teleop_thread.join();
    }

    if (trossen::utils::g_stop_requested) {
      std::cout << "\nStopping at user request (Ctrl+C).\n";
      break;
    }
  }

  // shutdown() calls stop_episode() (fires on_episode_ended) then on_pre_shutdown
  mgr.shutdown();

  const auto final_stats = mgr.stats();
  std::vector<std::string> extra_info = {
    "Data streams:         " +
      std::to_string(cfg.hardware.arms.size()) + " arms + " +
      std::to_string(cfg.hardware.cameras.size()) + " cameras"
  };
  trossen::utils::print_final_summary(
    final_stats.total_episodes_completed, root, extra_info);

  return 0;
}
