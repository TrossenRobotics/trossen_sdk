/**
 * @file trossen_vr_stationary.cpp
 * @brief VR-driven stationary demo — 2 VR controllers + 2 follower arms + 4 cameras.
 *
 * Left and right VR controllers each act as a Cartesian-space leader for
 * their respective follower arm. The hand-grip (hand trigger) is the deadman switch
 * per arm — the arm mirrors only while the hand-grip is held, and re-gripping
 * re-anchors the offset with no jump.
 *
 * VR button bindings (configurable under vr.session_control_{right,left}.bindings):
 *   Right A = start episode / advance to next / skip reset
 *   Right B = re-record current or last episode
 *   Left  X = stop current episode early (save partial)
 *   Left  Y = stop the whole session
 *
 * Usage:
 *   ./trossen_vr_stationary [OPTIONS]
 *
 *   --config PATH       Path to robot config JSON
 *                       [default: examples/trossen_vr_stationary/config.json]
 *   --set KEY=VALUE     Override a config value using dot notation (repeatable)
 *   --dump-config       Print merged config as JSON and exit
 *   --help              Show this help and exit
 *
 * Examples:
 *   ./trossen_vr_stationary
 *   ./trossen_vr_stationary --set hardware.arms.follower_right.ip_address=192.168.1.4
 *   ./trossen_vr_stationary --set session.max_duration=30
 *   ./trossen_vr_stationary --dump-config
 */

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"
#include "trossen_sdk/configuration/cli_parser.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/configuration/sdk_config.hpp"
#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/teleop/teleop_factory.hpp"
#include "trossen_sdk/hw/vr/vr_arm_component.hpp"
#include "trossen_sdk/hw/vr/vr_session_control_component.hpp"
#include "trossen_sdk/observer/observer_registry.hpp"
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
    "                     [default: examples/trossen_vr_stationary/config.json]\n"
    "  --set KEY=VALUE    Override a config value using dot notation (repeatable)\n"
    "  --dump-config      Print merged config as JSON and exit\n"
    "  --help             Show this help and exit\n"
    "\n"
    "Examples:\n"
    "  " << program << "\n"
    "  " << program << " --set hardware.arms.follower_right.ip_address=192.168.1.4\n"
    "  " << program << " --set session.max_duration=30\n"
    "  " << program << " --dump-config\n";
}

int main(int argc, char** argv) {
  // Flush every line immediately so progress is always visible.
  std::cout << std::unitbuf;
  trossen::configuration::CliParser cli(argc, argv);

  if (cli.has_flag("help")) {
    print_usage(argv[0]);
    return 0;
  }

  const std::string config_path = cli.get_string(
    "config", "examples/trossen_vr_stationary/config.json");

  if (!std::filesystem::exists(config_path)) {
    std::cerr << "Error: config file not found: " << config_path << "\n";
    return 1;
  }

  auto j = trossen::configuration::JsonLoader::load(config_path);
  const auto overrides = cli.get_set_overrides();
  if (!overrides.empty()) {
    j = trossen::configuration::merge_overrides(j, overrides);
  }

  if (cli.has_flag("dump-config")) {
    trossen::configuration::dump_config(j, "Trossen VR Stationary Config");
    return 0;
  }

  // Parse unified robot config
  auto cfg = trossen::configuration::SdkConfig::from_json(j);

  // Populate GlobalConfig so SessionManager picks up session + backend settings
  cfg.populate_global_config();

  const std::string root = cfg.mcap_backend.root;

  // Extract VR config block early — used in banner and hardware init.
  const auto vr_cfg       = j.contains("vr") ? j.at("vr") : nlohmann::json::object();
  const auto arm_ctrl_cfg = vr_cfg.contains("arm_controllers")
    ? vr_cfg.at("arm_controllers") : nlohmann::json::object();

  float joint_rate_hz = 30.0f;
  float camera_fps    = 30.0f;
  for (const auto& p : cfg.producers) {
    if (p.type == "trossen_arm") {
      joint_rate_hz = p.poll_rate_hz;
      break;
    }
  }
  for (const auto& p : cfg.producers) {
    if (p.type == "zed_camera" || p.type == "realsense_camera" ||
        p.type == "opencv_camera") { camera_fps = p.poll_rate_hz; break; }
  }

  std::vector<std::string> config_lines = {
    "Config file:          " + config_path,
    "Root directory:       " + root,
    "Backend:              " + cfg.session.backend_type,
    "Robot name:           " + cfg.robot_name,
  };
  for (const auto& [id, arm] : cfg.hardware.arms) {
    config_lines.push_back(
      "Arm [" + id + "]:  " + arm.ip_address + " (" + arm.end_effector + ")");
  }
  for (const auto& cam : cfg.hardware.cameras) {
    config_lines.push_back(
      "Camera [" + cam.id + "]:  " + cam.serial_number + "  " +
      std::to_string(cam.width) + "x" + std::to_string(cam.height) +
      " @ " + std::to_string(cam.fps) + " fps");
  }
  for (const auto& [id, entry] : arm_ctrl_cfg.items()) {
    config_lines.push_back(
      "VR ctrl [" + id + "]:       " +
      entry.value("controller_type", std::string{"right"}) + " controller, port " +
      std::to_string(entry.value("vr_port", 9000)));
  }
  config_lines.push_back("Joint rate:           " + std::to_string(joint_rate_hz) + " Hz");
  if (!cfg.hardware.cameras.empty()) {
    config_lines.push_back("Camera rate:          " + std::to_string(camera_fps) + " fps");
  }
  config_lines.push_back(
    "Teleop:               " +
    std::string(cfg.teleop.enabled ? "enabled" : "disabled") +
    " (" + std::to_string(cfg.teleop.pairs.size()) + " pairs)");

  trossen::utils::print_config_banner(
    "Trossen VR Stationary Demo", config_lines);

  trossen::utils::install_signal_handler();
  std::filesystem::create_directories(root);

  // ── Initialize follower arm(s) ───────────────────────────────────────────

  std::cout << "Initializing hardware...\n";

  std::unordered_map<std::string,
    std::shared_ptr<trossen::hw::arm::TrossenArmComponent>> arm_components;
  for (const auto& [id, arm_cfg] : cfg.hardware.arms) {
    auto component = trossen::hw::HardwareRegistry::create(
      "trossen_arm", id, arm_cfg.to_json(), true);
    arm_components[id] =
      std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(component);
    std::cout << "  [ok] Arm [" << id << "] configured ("
              << arm_cfg.ip_address << ")\n";
  }

  std::uint16_t vr_port = 9000;
  for (const auto& [_, entry] : arm_ctrl_cfg.items()) {
    if (entry.contains("vr_port")) {
      vr_port = entry.at("vr_port").get<std::uint16_t>();
      break;
    }
  }

  std::cout << "  [info] VR receiver on port " << vr_port
            << " — connect the VR app to this host's IP.\n";

  // ── Initialize VR arm controllers ───────────────────────────────────────
  // Each VrArmComponent is registered into ActiveHardwareRegistry under its
  // config id so the teleop factory pairs it with the follower arm.

  std::vector<std::shared_ptr<trossen::hw::vr::VrArmComponent>> vr_components;
  for (const auto& [id, entry] : arm_ctrl_cfg.items()) {
    auto component = trossen::hw::HardwareRegistry::create(
      "vr_arm_component", id, entry, true);
    vr_components.push_back(
      std::dynamic_pointer_cast<trossen::hw::vr::VrArmComponent>(component));
    std::cout << "  [ok] VR arm component [" << id << "] ("
              << entry.value("controller_type", std::string{"right"})
              << " controller)\n";
  }

  // ── Initialize cameras ──────────────────────────────────────────────────

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

  // ── Initialize VR session controls ──────────────────────────────────────
  // Right controller: A=start/advance, B=re-record.
  // Left  controller: X=stop-early, Y=stop-session.
  // Both share the same VrSession (one network connection); each claims only
  // its own buttons so there is no input conflict.

  std::vector<std::shared_ptr<trossen::hw::vr::VrSessionControlComponent>> session_ctrls;
  auto add_session_ctrl = [&](const char* cfg_key, const char* id) {
    if (!vr_cfg.contains(cfg_key)) return;
    const auto& sc_cfg = vr_cfg.at(cfg_key);
    auto component = trossen::hw::HardwareRegistry::create(
      "vr_session_control", id, sc_cfg, true);
    auto sc = std::dynamic_pointer_cast<trossen::hw::vr::VrSessionControlComponent>(component);
    session_ctrls.push_back(sc);
    std::cout << "  [ok] VR session control [" << id << "] ("
              << sc_cfg.value("controller_type", std::string{"right"}) << " controller)\n";
  };
  add_session_ctrl("session_control_right", "vr_session_right");
  add_session_ctrl("session_control_left",  "vr_session_left");

  // ── Session manager + producers ─────────────────────────────────────────

  trossen::runtime::SessionManager mgr;
  std::cout << "\nInitialized Session Manager\n"
               "  Starting episode index: "
            << mgr.stats().current_episode_index << "\n";
  if (mgr.stats().current_episode_index > 0) {
    std::cout << "  (Resuming from existing episodes in directory)\n";
  }
  std::cout << "\n";

  std::cout << "Creating producers...\n";
  for (const auto& prod_cfg : cfg.producers) {
    const auto period = std::chrono::milliseconds(
      static_cast<int>(1000.0f / prod_cfg.poll_rate_hz));
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

  // ── Observers ─────────────────────────────────────────────────────────

  if (!cfg.observers.empty()) {
    std::cout << "Creating observers...\n";
    for (const auto& obs_cfg : cfg.observers) {
      if (!obs_cfg.enabled) {
        std::cout << "  [disabled] Observer [" << obs_cfg.id << "] type=" << obs_cfg.type
                  << " (skipped: enabled=false)\n";
        continue;
      }
      try {
        auto obs = trossen::observer::ObserverRegistry::create(
          obs_cfg.type, obs_cfg.raw_json);
        mgr.add_observer(obs);
        std::cout << "  [ok] Observer [" << obs_cfg.id << "] type=" << obs_cfg.type
                  << " subscriptions=" << obs_cfg.subscriptions.size() << "\n";
      } catch (const std::exception& e) {
        std::cerr << "  [skip] Observer [" << obs_cfg.id << "] type=" << obs_cfg.type
                  << " failed to construct: " << e.what() << "\n";
        const auto types = trossen::observer::ObserverRegistry::get_registered_types();
        std::cerr << "         Registered observer types:";
        for (const auto& t : types) std::cerr << " " << t;
        std::cerr << "\n";
        if (!trossen::observer::ObserverRegistry::is_registered(obs_cfg.type)) {
          std::cerr << "         Hint: type '" << obs_cfg.type
                    << "' is not registered. Rebuild with the matching CMake option "
                    << "(e.g. -DTROSSEN_ENABLE_RERUN_OBSERVER=ON for the 'rerun' type) "
                    << "or set 'enabled': false on this observer to silence the warning.\n";
        }
      }
    }
  }

  // ── Teleop controllers ──────────────────────────────────────────────────

  auto controllers = trossen::hw::teleop::create_controllers_from_global_config();

  const bool has_teleop = !controllers.empty();
  for (auto& ctrl : controllers) mgr.add_teleop(std::move(ctrl));

  // ── Wire VR session control ─────────────────────────────────────────────
  // attach_control() installs callbacks that only post events onto a
  // thread-safe queue, starts each reader thread, and remembers the source so
  // shutdown() stops it. The session is only mutated on the main loop thread.
  if (!session_ctrls.empty()) {
    // Read the connection timeout from whichever session-control block is
    // present (right preferred), falling back to a default.
    const auto& sc_timeout_cfg = vr_cfg.contains("session_control_right")
      ? vr_cfg.at("session_control_right")
      : vr_cfg.at("session_control_left");
    const double sc_timeout = sc_timeout_cfg.value("connection_timeout_s", 120.0);
    std::cout << "\nWaiting for VR headset on port " << vr_port
              << " (timeout " << static_cast<int>(sc_timeout)
              << " s) — open the VR app and connect to this host's IP...\n";

    try {
      for (auto& sc : session_ctrls) mgr.attach_control(sc);
      std::cout << "VR headset connected. "
                   "Right A=start B=re-record, Left X=stop-early Y=stop-session.\n";
    } catch (const std::exception& e) {
      std::cerr << "\nError: VR headset did not connect — " << e.what() << "\n"
                << "Open the VR app and connect to this host's IP, then re-run.\n";
      mgr.shutdown();
      return 1;
    }
  }

  // ── Lifecycle callbacks ─────────────────────────────────────────────────

  int depth_cameras = 0;
  for (const auto& cam : cfg.hardware.cameras) {
    if (cam.use_depth) ++depth_cameras;
  }

  mgr.on_episode_ended([&](const trossen::runtime::SessionManager::Stats& stats) {
    const std::string file_path =
      trossen::utils::generate_episode_path(
        root + "/" + cfg.mcap_backend.dataset_id,
        stats.current_episode_index,
        "mcap");
    trossen::utils::print_episode_summary(file_path, stats);

    trossen::utils::SanityCheckConfig sanity_cfg{
      stats.elapsed.count(),
      static_cast<int>(cfg.hardware.arms.size()),
      joint_rate_hz,
      static_cast<int>(cfg.hardware.cameras.size()),
      static_cast<int>(camera_fps),
      5.0,
      depth_cameras,
      static_cast<int>(cfg.hardware.arms.size())  // one EEF pose producer per arm
    };
    perform_sanity_check(stats.current_episode_index, stats.records_written_current, sanity_cfg);
  });

  mgr.on_pre_shutdown([&]() {
    if (!has_teleop) {
      for (auto& [id, arm] : arm_components) arm->end_teleop();
    }
  });

  // ── Episode loop ────────────────────────────────────────────────────────

  std::cout << "\nReady. Press A on the right VR controller (or ENTER) to start.\n"
            << "  Squeeze and hold each hand-grip trigger to mirror the respective arm.\n"
            << "  Right A / ENTER      = start episode / advance to next / skip reset\n"
            << "  Right B / LEFT arrow = re-record current or last episode\n"
            << "  Left  X              = stop current episode early (save partial)\n"
            << "  Left  Y / Ctrl+C     = end the whole session\n\n";

  // Wait for explicit A / ENTER before the very first episode.
  {
    const auto initial = mgr.wait_for_reset();
    if (initial == trossen::runtime::UserAction::kStop ||
        trossen::utils::g_stop_requested) {
      std::cout << "\nAborted before first episode.\n";
      mgr.shutdown();
      return 0;
    }
  }

  while (true) {
    if (trossen::utils::g_stop_requested) {
      std::cout << "\nStopping at user request (Ctrl+C).\n";
      break;
    }

    mgr.print_episode_header();

    if (!mgr.start_episode()) break;
    std::cout << "Recording...\n";

    auto action = mgr.monitor_episode();
    if (action == trossen::runtime::UserAction::kReRecord) {
      mgr.discard_current_episode();
      continue;
    }
    if (mgr.is_episode_active()) mgr.stop_episode();
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
      std::to_string(cfg.hardware.arms.size()) + " arm(s) + " +
      std::to_string(cfg.hardware.cameras.size()) + " camera(s)",
    "VR controllers:       " + std::to_string(vr_components.size())
  };
  trossen::utils::print_final_summary(
    final_stats.total_episodes_completed, root, extra_info);

  return 0;
}
