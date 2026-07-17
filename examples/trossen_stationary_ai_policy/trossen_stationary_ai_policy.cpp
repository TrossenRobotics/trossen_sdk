/**
 * @file trossen_stationary_ai_policy.cpp
 * @brief Bimanual stationary kit driven by a remote policy server.
 *
 * Two follower arms + four RealSense cameras. Leaders are virtual: a
 * PolicyClient connects to a remote policy server, streams the follower joint
 * state and camera frames as observations, and exposes per-arm Face adapters
 * that the teleop machinery treats as drop-in leaders for the followers.
 *
 * This single source builds into two binaries that differ only in their
 * default config (which selects the transport):
 *   - trossen_stationary_ai_openpi  -> configs/openpi.json  (openpi_ws)
 *   - trossen_stationary_ai_lerobot -> configs/lerobot.json (lerobot_grpc)
 * The transport, server URL, and policy contract all live in that config; the
 * C++ is transport-agnostic. The default config path and the display label are
 * injected per target via the TROSSEN_AI_EXAMPLE_CONFIG / _LABEL compile
 * definitions (see examples/CMakeLists.txt); the fallbacks below keep the file
 * compiling standalone (e.g. for clangd).
 *
 * Usage:
 *   ./trossen_stationary_ai_<transport> [OPTIONS]
 *
 *   --config PATH       Path to robot config JSON [default: per-target]
 *   --set KEY=VALUE     Override a config value using dot notation (repeatable)
 *   --dump-config       Print merged config as JSON and exit
 *   --help              Show this help and exit
 */

#ifndef TROSSEN_AI_EXAMPLE_CONFIG
#define TROSSEN_AI_EXAMPLE_CONFIG \
  "examples/trossen_stationary_ai_policy/configs/openpi.json"
#endif
#ifndef TROSSEN_AI_EXAMPLE_LABEL
#define TROSSEN_AI_EXAMPLE_LABEL "openpi"
#endif

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"
#include "nlohmann/json.hpp"
#include "trossen_sdk/configuration/cli_parser.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/configuration/sdk_config.hpp"
#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/arm/trossen_arm_producer.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/policy/policy_client.hpp"
#include "trossen_sdk/hw/teleop/teleop_factory.hpp"
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
    "                     [default: " TROSSEN_AI_EXAMPLE_CONFIG "]\n"
    "  --set KEY=VALUE    Override a config value using dot notation (repeatable)\n"
    "  --dump-config      Print merged config as JSON and exit\n"
    "  --help             Show this help and exit\n"
    "\n"
    "Examples:\n"
    "  " << program << "\n"
    "  " << program << " --config " TROSSEN_AI_EXAMPLE_CONFIG "\n"
    "  " << program << " --set session.max_duration=30\n"
    "  " << program << " --set backend.dataset_id=my_demo\n"
    "  " << program << " --dump-config\n"
    "\n"
    "Note: --set uses dot-notation over JSON map keys only. Fields inside\n"
    "hardware.policy_clients[] must be edited directly in the config file.\n";
}

int main(int argc, char** argv) {
  trossen::configuration::CliParser cli(argc, argv);

  if (cli.has_flag("help")) {
    print_usage(argv[0]);
    return 0;
  }

  const std::string config_path =
    cli.get_string("config", TROSSEN_AI_EXAMPLE_CONFIG);

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
    trossen::configuration::dump_config(
      j, "Trossen Stationary AI Policy (" TROSSEN_AI_EXAMPLE_LABEL ") Config");
    return 0;
  }

  auto cfg = trossen::configuration::SdkConfig::from_json(j);
  cfg.populate_global_config();

  if (cfg.hardware.policy_clients.empty()) {
    std::cerr << "Error: hardware.policy_clients must contain at least one entry.\n";
    return 1;
  }

  std::unordered_set<std::string> policy_leader_ids;
  for (const auto& pc : cfg.hardware.policy_clients) {
    for (const auto& entry : pc.joint_layout) {
      policy_leader_ids.insert(entry.leader_id);
    }
  }
  for (const auto& pair : cfg.teleop.pairs) {
    if (policy_leader_ids.find(pair.leader) == policy_leader_ids.end()) {
      std::cerr << "Error: teleop.pair leader '" << pair.leader
                << "' is not provided by any policy_clients entry "
                << "(this example uses virtual leaders only).\n";
      return 1;
    }
  }

  const std::string root = cfg.mcap_backend.root;

  float joint_rate_hz = 30.0f;
  float camera_fps = 30.0f;
  float policy_action_rate_hz = 30.0f;
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
    if (p.type == "policy_client") {
      policy_action_rate_hz = p.poll_rate_hz;
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
  for (const auto& pc : cfg.hardware.policy_clients) {
    config_lines.push_back(
      "Policy [" + pc.id + "]:  " + pc.server_url +
      "  (inference=" + std::to_string(pc.inference_hz) + " Hz, " +
      std::to_string(pc.joint_layout.size()) + " face(s))");
  }
  config_lines.push_back("Joint rate:           " + std::to_string(joint_rate_hz) + " Hz");
  config_lines.push_back(
    "Policy action rate:   " + std::to_string(policy_action_rate_hz) + " Hz");
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

  trossen::utils::print_config_banner(
    "Trossen Stationary AI Policy (" TROSSEN_AI_EXAMPLE_LABEL ") Demo Usage",
    config_lines);

  trossen::utils::install_signal_handler();
  std::filesystem::create_directories(root);

  // ──────────────────────────────────────────────────────────
  // Follower arms
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

  // ──────────────────────────────────────────────────────────
  // Cameras
  // ──────────────────────────────────────────────────────────

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

  // ──────────────────────────────────────────────────────────
  // PolicyClient(s)
  // ──────────────────────────────────────────────────────────

  std::unordered_map<std::string,
    std::shared_ptr<trossen::hw::policy::PolicyClient>> policy_clients;

  trossen::runtime::SessionManager mgr;
  std::cout << "\nInitialized Session Manager\n";
  std::cout << "  Starting episode index: " << mgr.stats().current_episode_index << "\n";
  if (mgr.stats().current_episode_index > 0) {
    std::cout << "  (Resuming from existing episodes in directory)\n";
  }
  std::cout << "\n";

  for (const auto& pc_cfg : cfg.hardware.policy_clients) {
    auto hw_component = trossen::hw::HardwareRegistry::create(
      "policy_client", pc_cfg.id, pc_cfg.raw_json);
    auto client =
      std::dynamic_pointer_cast<trossen::hw::policy::PolicyClient>(hw_component);
    if (!client) {
      std::cerr << "Error: hardware component '" << pc_cfg.id
                << "' is not a PolicyClient.\n";
      return 1;
    }
    client->set_control_rate_hz(policy_action_rate_hz);
    // Start paused; lifecycle callbacks resume per-episode.
    client->set_inference_active(false);
    policy_clients[pc_cfg.id] = client;
    mgr.add_observer(client);
    std::cout << "  [ok] PolicyClient [" << pc_cfg.id << "] "
              << pc_cfg.server_url << " (faces=" << client->faces().size() << ")\n";
  }

  // ──────────────────────────────────────────────────────────
  // Producers
  // ──────────────────────────────────────────────────────────

  std::cout << "Creating producers...\n";

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
    } else if (prod_cfg.type == "policy_client") {
      auto prod = trossen::runtime::ProducerRegistry::create(
        "policy_client",
        policy_clients.at(prod_cfg.hardware_id),
        prod_cfg.to_registry_json());
      mgr.add_producer(prod, period);
      std::cout << "  [ok] Policy producer [" << prod_cfg.stream_id << "] ("
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
  // Observers (rerun, etc.) — registered once, started lazily on first episode.
  // ──────────────────────────────────────────────────────────

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
        std::cerr << "Observer [" << obs_cfg.id << "] type=" << obs_cfg.type
                  << " failed to construct: " << e.what() << "\n";
        const auto types =
          trossen::observer::ObserverRegistry::get_registered_types();
        std::cerr << "Registered observer types:";
        for (const auto& t : types) std::cerr << " " << t;
        std::cerr << "\n";
        if (!trossen::observer::ObserverRegistry::is_registered(obs_cfg.type)) {
          std::cerr << "Hint: type '" << obs_cfg.type
                    << "' is not registered. Rebuild with the matching CMake option "
                    << "(e.g. -DTROSSEN_ENABLE_RERUN_OBSERVER=ON for the 'rerun' type).\n";
        }
        return 1;
      }
    }
  }

  // ──────────────────────────────────────────────────────────
  // Teleop controllers
  // ──────────────────────────────────────────────────────────

  auto controllers = trossen::hw::teleop::create_controllers_from_global_config();

  // No manual wait for staging is needed here: each arm homes synchronously to
  // its staged_position via the blocking on_pre_episode()/stage() path inside
  // start_episode(), so the followers are already settled before mirroring begins.

  // ──────────────────────────────────────────────────────────
  // Lifecycle callbacks
  // ──────────────────────────────────────────────────────────

  // Policy-readiness gate. on_episode_started sets policy_ready true once every
  // policy client has produced its first action chunk; it stays false if none
  // arrives within kPolicyReadyTimeout. The episode loop discards and stops when
  // it stays false, so a non-responsive policy never saves an episode of the
  // followers frozen at the staged pose. policy_urls names the servers for the
  // error message.
  constexpr auto kPolicyReadyTimeout = std::chrono::seconds(30);
  bool policy_ready = false;
  std::string policy_urls;
  for (const auto& pc : cfg.hardware.policy_clients) {
    if (!policy_urls.empty()) policy_urls += ", ";
    policy_urls += pc.id + " (" + pc.server_url + ")";
  }

  mgr.on_pre_episode([&]() -> bool {
    for (auto& ctrl : controllers) ctrl->prepare_teleop();
    return true;
  });

  mgr.on_episode_started([&]() {
    // Wake the inference threads for this episode. Each PolicyClient was
    // started paused so no round-trips fire between episodes.
    for (auto& [id, client] : policy_clients) {
      client->set_inference_active(true);
    }

    // Hold staged pose until the first policy chunk lands; otherwise the
    // mirror loop would write hold-last-action zeros to the followers and
    // jerk them toward the rest pose.
    std::cout << "Waiting for first policy chunk before starting mirror...\n";
    const auto deadline = std::chrono::steady_clock::now() + kPolicyReadyTimeout;
    policy_ready = false;
    // Honour Ctrl+C while waiting: the arms are energized here, so the loop
    // must exit promptly on a stop request instead of blocking for the full
    // timeout.
    while (std::chrono::steady_clock::now() < deadline &&
           !trossen::utils::g_stop_requested) {
      bool all_ready = true;
      for (const auto& [id, client] : policy_clients) {
        if (!client->latest_chunk()) {
          all_ready = false;
          break;
        }
      }
      if (all_ready) {
        policy_ready = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Start mirroring only after a real chunk has landed: kicking off teleop on
    // the zero-fill hold-last-action row would jerk the followers off the staged
    // pose toward rest. When no chunk arrives, leave the followers on the held
    // staged pose (safe); the episode loop discards this episode and stops.
    if (policy_ready) {
      for (auto& ctrl : controllers) ctrl->teleop();
      std::cout << "Episode started - recording"
                << (controllers.empty() ? "" : " and policy-driven mirroring")
                << " active.\n";
    }
  });

  int depth_cameras = 0;
  for (const auto& cam : cfg.hardware.cameras) {
    if (cam.use_depth) ++depth_cameras;
  }

  mgr.on_episode_ended([&](const trossen::runtime::SessionManager::Stats& stats) {
    // Pause inference for the reset window; resumes in on_episode_started.
    for (auto& [id, client] : policy_clients) {
      client->set_inference_active(false);
    }

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

  mgr.on_pre_shutdown([&]() {
    for (auto& ctrl : controllers) ctrl->stop_teleop();
    if (controllers.empty()) {
      for (auto& [id, arm] : arm_components) arm->end_teleop();
    }
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

    // Policy-readiness gate: on_episode_started waited for the first action
    // chunk from every policy client. If none arrived, the episode holds the
    // followers at the staged pose with no policy motion, so discard it (nothing
    // usable was captured) and stop rather than record more empty episodes.
    if (!policy_ready) {
      mgr.discard_current_episode();
      if (trossen::utils::g_stop_requested) {
        std::cout << "\nStopping at user request (Ctrl+C).\n";
      } else {
        std::cerr << "\nERROR: no policy returned an action within "
                  << kPolicyReadyTimeout.count()
                  << "s — the policy server is not responding. Discarded the "
                     "empty episode and stopping. Check that the policy "
                     "server(s) are running and serving the configured "
                     "checkpoint: " << policy_urls << "\n";
      }
      break;
    }

    std::cout << "Recording...\n";

    auto action = mgr.monitor_episode();

    if (action == trossen::runtime::UserAction::kReRecord) {
      mgr.discard_current_episode();
      continue;
    }

    if (mgr.is_episode_active()) {
      mgr.stop_episode();
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

  mgr.shutdown();

  const auto final_stats = mgr.stats();
  std::vector<std::string> extra_info = {
    "Data streams:         " +
      std::to_string(cfg.hardware.arms.size()) + " arms + " +
      std::to_string(cfg.hardware.cameras.size()) + " cameras + " +
      std::to_string(cfg.hardware.policy_clients.size()) + " policy client(s)"
  };
  trossen::utils::print_final_summary(
    final_stats.total_episodes_completed, root, extra_info);

  // All SDK-owned teardown — arm safing (end_teleop), teleop stop, and the
  // recording flush/close — has completed inside mgr.shutdown() above: the
  // dataset is finalized and the arms are at rest. Exit here instead of running
  // the process through third-party library static/destructor teardown, which
  // is unreliable on this platform (the RealSense pipeline destructor can abort
  // during close). Nothing the SDK owns is left to release.
  std::cout.flush();
  std::cerr.flush();
  std::_Exit(0);
}
