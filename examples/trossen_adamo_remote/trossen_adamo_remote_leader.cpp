/**
 * @file trossen_adamo_remote_leader.cpp
 * @brief Remote-teleop leader/publisher: drive a local leader arm in
 *        gravity-compensation and stream its joint state to Adamo. EXPERIMENTAL.
 *
 * This is the publish half of the remote-teleop loop. It owns the physical
 * leader arm, puts it into gravity-compensation so the operator can back-drive
 * it, polls its joint state with a TrossenArmProducer, and an AdamoObserver
 * publishes each sample to `<robot>/<arm>/state`. The follower half
 * (`trossen_adamo_remote_follower`) subscribes to that topic and mirrors it onto
 * a local follower arm.
 *
 * There is NO teleoperation on this side: the leader is a pure source. Rather
 * than fabricate a leader-only teleop pair just to reach gravity-comp, this
 * binary calls the arm's lifecycle directly:
 *   - prepare_for_teleop()  -> external_effort gravity-comp (operator can move it)
 *   - end_teleop()          -> neutralize + return to rest at shutdown
 * (Both are part of the public TrossenArmComponent lifecycle; see
 *  src/hw/arm/trossen_arm_component.cpp.)
 *
 * Requires `-DTROSSEN_ENABLE_ADAMO=ON` and `ADAMO_API_KEY` set. Start this
 * BEFORE the follower (the follower blocks waiting for the first frame).
 *
 * Usage:
 *   ./trossen_adamo_remote_leader [OPTIONS]
 *
 *   --config PATH       Path to leader config JSON
 *                       [default: examples/trossen_adamo_remote/leader.config.json]
 *   --set KEY=VALUE     Override a config value using dot notation (repeatable)
 *   --dump-config       Print merged config as JSON and exit
 *   --help              Show this help and exit
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
#include "trossen_sdk/observer/observer_registry.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"

#include "trossen_sdk/utils/app_utils.hpp"

static void print_usage(const char* program) {
  std::cout <<
    "Usage: " << program << " [OPTIONS]\n"
    "\n"
    "Stream a local leader arm's joint state to Adamo for remote teleop.\n"
    "\n"
    "Options:\n"
    "  --config PATH      Path to leader config JSON\n"
    "                     [default: examples/trossen_adamo_remote/leader.config.json]\n"
    "  --set KEY=VALUE    Override a config value using dot notation (repeatable)\n"
    "  --dump-config      Print merged config as JSON and exit\n"
    "  --help             Show this help and exit\n"
    "\n"
    "Requires -DTROSSEN_ENABLE_ADAMO=ON and ADAMO_API_KEY in the environment.\n"
    "Start this BEFORE the follower.\n"
    "\n"
    "Examples:\n"
    "  " << program << "\n"
    "  " << program << " --config examples/trossen_adamo_remote/leader.config.json\n"
    "  " << program << " --set hardware.arms.leader.ip_address=192.168.1.2\n"
    "  " << program << " --dump-config\n";
}

int main(int argc, char** argv) {
  trossen::configuration::CliParser cli(argc, argv);

  if (cli.has_flag("help")) {
    print_usage(argv[0]);
    return 0;
  }

  const std::string config_path =
    cli.get_string("config", "examples/trossen_adamo_remote/leader.config.json");

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
    trossen::configuration::dump_config(j, "Trossen Adamo Remote Leader Config");
    return 0;
  }

  // Parse unified robot config
  auto cfg = trossen::configuration::SdkConfig::from_json(j);

  // Populate GlobalConfig so SessionManager picks up session + backend settings
  cfg.populate_global_config();

  const std::string root = cfg.mcap_backend.root;

  float joint_rate_hz = 30.0f;
  for (const auto& p : cfg.producers) {
    if (p.type == "trossen_arm") {
      joint_rate_hz = p.poll_rate_hz;
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
  config_lines.push_back(
    "Observers:            " + std::to_string(cfg.observers.size()) +
    " (publishing to Adamo)");

  trossen::utils::print_config_banner(
    "Trossen Adamo Remote Leader / Publisher (EXPERIMENTAL)", config_lines);

  trossen::utils::install_signal_handler();
  std::filesystem::create_directories(root);

  // ──────────────────────────────────────────────────────────
  // Initialize the leader arm hardware from config
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
  // Session Manager + producers
  // ──────────────────────────────────────────────────────────

  trossen::runtime::SessionManager mgr;
  std::cout << "\nInitialized Session Manager\n\n";

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
    }
  }

  std::cout << "\nProducers registered. Ready to publish.\n";

  // ──────────────────────────────────────────────────────────
  // Observers: the AdamoObserver that publishes the leader's joint state.
  // Registered once, started lazily on first episode, persists across episodes.
  // ──────────────────────────────────────────────────────────

  if (cfg.observers.empty()) {
    std::cerr << "Warning: no observers configured; this binary would drive the leader "
              << "into gravity-comp but publish nothing. Add an 'adamo' observer.\n";
  } else {
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
        const auto types =
          trossen::observer::ObserverRegistry::get_registered_types();
        std::cerr << "         Registered observer types:";
        for (const auto& t : types) std::cerr << " " << t;
        std::cerr << "\n";
        if (!trossen::observer::ObserverRegistry::is_registered(obs_cfg.type)) {
          std::cerr << "         Hint: type '" << obs_cfg.type
                    << "' is not registered. Rebuild with -DTROSSEN_ENABLE_ADAMO=ON.\n";
        }
        continue;
      }
    }
  }

  // ──────────────────────────────────────────────────────────
  // Lifecycle: engage gravity-comp so the operator can back-drive the leader,
  // then let the producer poll + observer publish. Released at shutdown.
  // ──────────────────────────────────────────────────────────

  bool leader_engaged = false;

  // on_pre_episode runs before the scheduler begins polling, so the arm is
  // back-driveable before we start reading it. Guarded so it engages once and
  // persists across episodes (matches how TeleopController prepares arms).
  mgr.on_pre_episode([&]() -> bool {
    if (!leader_engaged) {
      for (auto& [id, arm] : arm_components) {
        arm->prepare_for_teleop();
        std::cout << "  [leader] Arm [" << id << "] in gravity-comp — back-drive it.\n";
      }
      leader_engaged = true;
    }
    return true;
  });

  mgr.on_episode_started([&]() {
    std::cout << "Publishing leader state to Adamo. Move the leader arm.\n";
  });

  mgr.on_episode_ended([&](const trossen::runtime::SessionManager::Stats& stats) {
    const std::string file_path =
      trossen::utils::generate_episode_path(root, stats.current_episode_index);
    trossen::utils::print_episode_summary(file_path, stats);
  });

  // Shutdown: neutralize + return the leader to rest.
  mgr.on_pre_shutdown([&]() {
    for (auto& [id, arm] : arm_components) arm->end_teleop();
  });

  // ──────────────────────────────────────────────────────────
  // Episode loop (one long episode; Ctrl+C to stop)
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

    std::cout << "Publishing...\n";

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

  // shutdown() calls stop_episode() (no-op if already stopped) then on_pre_shutdown
  mgr.shutdown();

  const auto final_stats = mgr.stats();
  std::vector<std::string> extra_info = {
    "Published streams:    " + std::to_string(cfg.hardware.arms.size()) + " leader arm(s)"
  };
  trossen::utils::print_final_summary(
    final_stats.total_episodes_completed, root, extra_info);

  return 0;
}
