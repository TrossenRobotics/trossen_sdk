/**
 * @file trossen_vr_stationary.cpp
 * @brief VR-driven stationary AI demo — 1 follower arm + 1 Meta Quest leader.
 *
 * Differences from `trossen_stationary_ai`:
 *   - Leader arm(s) are replaced by a `VrArmControllerComponent` per VR hand.
 *     The controller's Cartesian pose + trigger drive the follower arm.
 *   - A new "start-signal gate" sits between hardware init and the first
 *     episode: the operator dons the Quest, then presses the A-button on
 *     the right controller (or ENTER in this terminal) to begin teleop.
 *     The same gate runs between episodes so the session can continue
 *     without leaving the headset.
 *
 * Usage:
 *   ./trossen_vr_stationary [OPTIONS]
 *
 *   --config PATH       Path to robot config JSON
 *                       [default: examples/trossen_vr_stationary/config.json]
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

#include "nlohmann/json.hpp"
#include "trossen_sdk/configuration/cli_parser.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/configuration/sdk_config.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/teleop/teleop_factory.hpp"
#include "trossen_sdk/hw/vr/vr_arm_controller.hpp"
#include "trossen_sdk/hw/vr/vr_session.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"
#include "trossen_sdk/utils/app_utils.hpp"
#include "trossen_sdk/utils/keyboard_input_utils.hpp"

namespace {

void print_usage(const char* program) {
  std::cout <<
    "Usage: " << program << " [OPTIONS]\n"
    "\n"
    "Options:\n"
    "  --config PATH      Path to robot config JSON\n"
    "                     [default: examples/trossen_vr_stationary/config.json]\n"
    "  --set KEY=VALUE    Override a config value using dot notation (repeatable)\n"
    "  --dump-config      Print merged config as JSON and exit\n"
    "  --help             Show this help and exit\n";
}

/// Primary controller hand used by the start-signal gate. Read off the first
/// VR arm controller so the A-button watched for "start" matches the hand
/// the operator is already holding out to drive the follower arm.
std::string pick_primary_hand(const nlohmann::json& vr_cfg) {
  if (vr_cfg.contains("arm_controllers") && vr_cfg["arm_controllers"].is_object()) {
    for (const auto& [_, entry] : vr_cfg["arm_controllers"].items()) {
      if (entry.contains("controller")) return entry["controller"].get<std::string>();
    }
  }
  return "right";
}

/// Block until the operator signals "start". Returns true on start, false if
/// the session is aborted (Ctrl+C). Prints a live status line so the
/// operator can see when the Quest connects.
bool wait_for_start_signal(const std::string& primary_hand) {
  auto& session = trossen::hw::vr::VrSession::instance();
  trossen::utils::RawModeGuard raw_mode;

  std::cout << "\nPut on the Meta Quest and launch the VR app.\n"
               "Press the A-button on the " << primary_hand
            << " controller (or ENTER in this terminal) to start teleoperation.\n"
               "(Ctrl+C to abort)\n\n";

  bool last_connected = false;
  while (!trossen::utils::g_stop_requested) {
    const bool connected = session.is_quest_connected();
    if (connected != last_connected) {
      std::cout << "  Quest: " << (connected ? "CONNECTED" : "waiting...")
                << "\n";
      last_connected = connected;
    }

    if (connected && session.consume_start_signal(primary_hand)) return true;

    const auto key = trossen::utils::poll_keypress();
    if (key == trossen::utils::KeyPress::kEnter) {
      // ENTER is the fallback for operators testing without the headset on.
      // Still require Quest connection so we never mirror a blank pose.
      if (connected) return true;
      std::cout << "  (ignored ENTER — Quest not yet connected)\n";
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
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

  // The `vr` block is demo-specific (the SDK config schema does not yet carry
  // VR hardware), so pull it out before handing the rest to SdkConfig.
  nlohmann::json vr_cfg = nlohmann::json::object();
  if (j.contains("vr")) vr_cfg = j["vr"];

  auto cfg = trossen::configuration::SdkConfig::from_json(j);
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
    "Robot name:           " + cfg.robot_name,
  };
  for (const auto& [id, arm] : cfg.hardware.arms) {
    config_lines.push_back(
      "Arm [" + id + "]:  " + arm.ip_address + " (" + arm.end_effector + ")");
  }
  if (vr_cfg.contains("arm_controllers")) {
    for (const auto& [id, entry] : vr_cfg["arm_controllers"].items()) {
      config_lines.push_back(
        "VR arm [" + id + "]:  " +
        entry.value("controller", std::string{"right"}) + " hand, port " +
        std::to_string(entry.value("vr_port", 5432)));
    }
  }
  config_lines.push_back("Joint rate:           " + std::to_string(joint_rate_hz) + " Hz");
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

  // ── Initialize VR arm controller(s) — binds the WebSocket port ──────────

  std::vector<std::shared_ptr<trossen::hw::vr::VrArmControllerComponent>>
    vr_components;
  if (vr_cfg.contains("arm_controllers")) {
    for (const auto& [id, entry] : vr_cfg["arm_controllers"].items()) {
      auto component = trossen::hw::HardwareRegistry::create(
        "vr_arm_controller", id, entry, true);
      vr_components.push_back(
        std::dynamic_pointer_cast<trossen::hw::vr::VrArmControllerComponent>(
          component));
      std::cout << "  [ok] VR arm controller [" << id << "] configured ("
                << entry.value("controller", std::string{"right"}) << " hand)\n";
    }
  }

  const std::string primary_hand = pick_primary_hand(vr_cfg);

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
    }
  }
  std::cout << "\nProducers registered. Ready to record.\n";

  // ── Teleop controllers ──────────────────────────────────────────────────

  auto controllers = trossen::hw::teleop::create_controllers_from_global_config();

  mgr.on_pre_episode([&]() -> bool {
    for (auto& ctrl : controllers) ctrl->prepare_teleop();
    return true;
  });
  mgr.on_episode_started([&]() {
    for (auto& ctrl : controllers) ctrl->teleop();
    std::cout << "Episode started - recording"
              << (controllers.empty() ? "" : " and mirroring") << " active.\n";
  });
  mgr.on_episode_ended([&](const trossen::runtime::SessionManager::Stats& stats) {
    for (auto& ctrl : controllers) ctrl->reset_teleop();
    const std::string file_path =
      trossen::utils::generate_episode_path(root, stats.current_episode_index);
    trossen::utils::print_episode_summary(file_path, stats);
  });
  mgr.on_pre_shutdown([&]() {
    for (auto& ctrl : controllers) ctrl->stop_teleop();
    if (controllers.empty()) {
      for (auto& [id, arm] : arm_components) arm->end_teleop();
    }
  });

  // ── Pre-session gate: wait for Quest + start signal ─────────────────────

  if (!wait_for_start_signal(primary_hand)) {
    std::cout << "\nAborted before first episode.\n";
    mgr.shutdown();
    return 0;
  }

  // ── Episode loop ────────────────────────────────────────────────────────

  while (true) {
    if (trossen::utils::g_stop_requested) {
      std::cout << "\n\nStopping at user request (Ctrl+C).\n";
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

    // Gate the next episode on the same start signal, so the operator can
    // stay in the headset and press A to continue.
    std::cout << "\nEpisode complete. Press A (or ENTER) to record the next "
                 "episode, or Ctrl+C to end the session.\n";
    if (!wait_for_start_signal(primary_hand)) break;
  }

  mgr.shutdown();

  const auto final_stats = mgr.stats();
  std::vector<std::string> extra_info = {
    "Data streams:         " +
      std::to_string(cfg.hardware.arms.size()) + " follower arm(s), " +
      std::to_string(vr_components.size()) + " VR leader(s)"
  };
  trossen::utils::print_final_summary(
    final_stats.total_episodes_completed, root, extra_info);

  return 0;
}
