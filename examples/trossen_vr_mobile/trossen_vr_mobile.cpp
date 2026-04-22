/**
 * @file trossen_vr_mobile.cpp
 * @brief VR-driven bimanual mobile AI demo — two follower arms + SLATE base.
 *
 * Extends `trossen_vr_stationary` with a second follower arm and a mobile
 * base, all driven from a single Meta Quest:
 *   - Left  VR controller → follower_left arm  (cartesian pose + gripper).
 *   - Right VR controller → follower_right arm (cartesian pose + gripper).
 *   - Left  VR thumbstick → SLATE base (linear forward, angular yaw).
 *   - Right VR buttons    → session control (A = start/advance, B = re-record,
 *                           grip = end session).
 *
 * All VR components share one process-wide VrSession (one WebSocket to the
 * Quest) and claim non-overlapping inputs via VrSession::claim_inputs(), so
 * conflicting configurations fail at configure() time with a clear error.
 *
 * Session-control events from the right-hand buttons are attached to the
 * SessionManager via `attach_session_control()` — no custom start-signal
 * gate is needed; `wait_for_reset()` does the work.
 *
 * Usage:
 *   ./trossen_vr_mobile [OPTIONS]
 *
 *   --config PATH       Path to robot config JSON
 *                       [default: examples/trossen_vr_mobile/config.json]
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
#include "trossen_sdk/hw/base/slate_base_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/teleop/teleop_factory.hpp"
#include "trossen_sdk/hw/vr/vr_arm_controller.hpp"
#include "trossen_sdk/hw/vr/vr_base_joystick.hpp"
#include "trossen_sdk/hw/vr/vr_mdns_advertiser.hpp"
#include "trossen_sdk/hw/vr/vr_session.hpp"
#include "trossen_sdk/hw/vr/vr_session_control.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"
#include "trossen_sdk/runtime/push_producer_registry.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"
#include "trossen_sdk/utils/app_utils.hpp"

namespace {

void print_usage(const char* program) {
  std::cout <<
    "Usage: " << program << " [OPTIONS]\n"
    "\n"
    "Options:\n"
    "  --config PATH      Path to robot config JSON\n"
    "                     [default: examples/trossen_vr_mobile/config.json]\n"
    "  --set KEY=VALUE    Override a config value using dot notation (repeatable)\n"
    "  --dump-config      Print merged config as JSON and exit\n"
    "  --help             Show this help and exit\n";
}

}  // namespace

int main(int argc, char** argv) {
  trossen::configuration::CliParser cli(argc, argv);

  if (cli.has_flag("help")) {
    print_usage(argv[0]);
    return 0;
  }

  const std::string config_path = cli.get_string(
    "config", "examples/trossen_vr_mobile/config.json");

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
    trossen::configuration::dump_config(j, "Trossen VR Mobile Config");
    return 0;
  }

  // The `vr` block is demo-specific (the SDK config schema does not yet
  // carry VR hardware), so pull it out before handing the rest to SdkConfig.
  nlohmann::json vr_cfg = nlohmann::json::object();
  if (j.contains("vr")) vr_cfg = j["vr"];

  auto cfg = trossen::configuration::SdkConfig::from_json(j);
  cfg.populate_global_config();

  const std::string root = cfg.mcap_backend.root;

  float joint_rate_hz = 30.0f;
  float base_rate_hz  = 30.0f;
  for (const auto& p : cfg.producers) {
    if (p.type == "trossen_arm") {
      joint_rate_hz = p.poll_rate_hz;
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
    "Robot name:           " + cfg.robot_name,
  };
  for (const auto& [id, arm] : cfg.hardware.arms) {
    config_lines.push_back(
      "Arm [" + id + "]:  " + arm.ip_address + " (" + arm.end_effector + ")");
  }
  if (cfg.hardware.mobile_base.has_value()) {
    config_lines.push_back("Mobile base:          SLATE (torque " +
      std::string(cfg.hardware.mobile_base->enable_torque ? "on" : "off") + ")");
  }
  for (const auto& cam : cfg.hardware.cameras) {
    config_lines.push_back(
      "Camera [" + cam.id + "]:  " + cam.serial_number + "  " +
      std::to_string(cam.width) + "x" + std::to_string(cam.height) +
      " @ " + std::to_string(cam.fps) + " fps");
  }
  if (vr_cfg.contains("arm_controllers")) {
    for (const auto& [id, entry] : vr_cfg["arm_controllers"].items()) {
      config_lines.push_back(
        "VR arm [" + id + "]:  " +
        entry.value("controller", std::string{"right"}) + " hand, port " +
        std::to_string(entry.value("vr_port", 5432)));
    }
  }
  if (vr_cfg.contains("base_joysticks")) {
    for (const auto& [id, entry] : vr_cfg["base_joysticks"].items()) {
      config_lines.push_back(
        "VR base [" + id + "]: " +
        entry.value("controller", std::string{"left"}) + " thumbstick, port " +
        std::to_string(entry.value("vr_port", 5432)));
    }
  }
  config_lines.push_back(
    "Joint rate:           " + std::to_string(joint_rate_hz) + " Hz");
  if (cfg.hardware.mobile_base.has_value()) {
    config_lines.push_back(
      "Mobile base rate:     " + std::to_string(base_rate_hz) + " Hz");
  }
  config_lines.push_back(
    "Teleop:               " +
    std::string(cfg.teleop.enabled ? "enabled" : "disabled") +
    " (" + std::to_string(cfg.teleop.pairs.size()) + " pairs)");

  trossen::utils::print_config_banner(
    "Trossen VR Mobile Demo", config_lines);

  trossen::utils::install_signal_handler();
  std::filesystem::create_directories(root);

  // ── Initialize follower arm(s) ──────────────────────────────────────────

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

  // ── Initialize SLATE mobile base ────────────────────────────────────────

  std::shared_ptr<trossen::hw::HardwareComponent> slate_base_component;
  if (cfg.hardware.mobile_base.has_value()) {
    // Using direct construction + explicit registration (same as
    // trossen_mobile_ai) — keeps the identifier consistent with the
    // teleop pair config ("slate_base").
    slate_base_component =
      std::make_shared<trossen::hw::base::SlateBaseComponent>("slate_base");
    slate_base_component->configure(cfg.hardware.mobile_base->to_json());
    trossen::hw::ActiveHardwareRegistry::register_active(
      "slate_base", slate_base_component);
    std::cout << "  [ok] SLATE mobile base configured\n";
  }

  // ── Advertise the host over mDNS so the Quest app can discover it ──────

  std::uint16_t vr_port = 5432;
  if (vr_cfg.contains("arm_controllers")) {
    for (const auto& [_, entry] : vr_cfg["arm_controllers"].items()) {
      if (entry.contains("vr_port")) {
        vr_port = entry.at("vr_port").get<std::uint16_t>();
        break;
      }
    }
  }

  trossen::hw::vr::VrMdnsAdvertiser mdns;
  try {
    mdns.start(vr_port, "TrossenVR");
    std::cout << "  [ok] mDNS advertising TrossenVR on port " << vr_port
              << " (_trossen-vr._tcp)\n";
  } catch (const std::exception& e) {
    std::cerr << "  [warn] mDNS advertisement failed: " << e.what() << "\n"
              << "         The Quest may not auto-discover this host; "
                 "connect manually by IP or run mdns_helper.py as a "
                 "fallback.\n";
  }

  // ── Initialize VR leaders (binds shared WebSocket port) ─────────────────

  std::vector<std::shared_ptr<trossen::hw::vr::VrArmControllerComponent>>
    vr_arm_components;
  if (vr_cfg.contains("arm_controllers")) {
    for (const auto& [id, entry] : vr_cfg["arm_controllers"].items()) {
      auto component = trossen::hw::HardwareRegistry::create(
        "vr_arm_controller", id, entry, true);
      vr_arm_components.push_back(
        std::dynamic_pointer_cast<trossen::hw::vr::VrArmControllerComponent>(
          component));
      std::cout << "  [ok] VR arm controller [" << id << "] ("
                << entry.value("controller", std::string{"right"})
                << " hand)\n";
    }
  }

  std::vector<std::shared_ptr<trossen::hw::vr::VrBaseJoystickComponent>>
    vr_base_components;
  if (vr_cfg.contains("base_joysticks")) {
    for (const auto& [id, entry] : vr_cfg["base_joysticks"].items()) {
      auto component = trossen::hw::HardwareRegistry::create(
        "vr_base_joystick", id, entry, true);
      vr_base_components.push_back(
        std::dynamic_pointer_cast<trossen::hw::vr::VrBaseJoystickComponent>(
          component));
      std::cout << "  [ok] VR base joystick [" << id << "] ("
                << entry.value("controller", std::string{"left"})
                << " thumbstick)\n";
    }
  }

  // VR session-control component — buttons on the Quest drive episode
  // start / re-record / stop-session through the SessionManager's
  // SessionControlCapable channel, so no custom start-signal gate.
  std::shared_ptr<trossen::hw::vr::VrSessionControlComponent> session_control;
  if (vr_cfg.contains("session_control")) {
    const auto& entry = vr_cfg["session_control"];
    auto component = trossen::hw::HardwareRegistry::create(
      "vr_session_control", "vr_session_control", entry, true);
    session_control =
      std::dynamic_pointer_cast<trossen::hw::vr::VrSessionControlComponent>(
        component);
    std::cout << "  [ok] VR session control configured ("
              << entry.value("controller", std::string{"right"}) << " hand)\n";
  }

  // ── Initialize cameras ──────────────────────────────────────────────────
  // Create each camera hardware component ahead of producer construction so
  // the producer loop below can look them up by id. Cameras auto-register
  // in ActiveHardwareRegistry (4th arg default), but we also keep a local
  // map so the producer branch avoids a second registry lookup per entry.

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

  // Kept alive beyond the producer-creation loop: the velocity polling
  // thread pokes it during each episode so the slate driver stays warm
  // even between the session manager's scheduled polls (matches the
  // pattern used in trossen_mobile_ai).
  std::shared_ptr<trossen::hw::PolledProducer> base_prod;

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
    } else if (prod_cfg.type == "slate_base" && slate_base_component) {
      base_prod = trossen::runtime::ProducerRegistry::create(
        "slate_base",
        slate_base_component,
        prod_cfg.to_registry_json());
      mgr.add_producer(base_prod, period);
      std::cout << "  [ok] Mobile base producer ("
                << prod_cfg.poll_rate_hz << " Hz)\n";
    } else if (camera_components.count(prod_cfg.hardware_id)) {
      const auto* cam = camera_cfg_map.at(prod_cfg.hardware_id);
      if (trossen::runtime::PushProducerRegistry::is_registered(prod_cfg.type)) {
        auto prod = trossen::runtime::PushProducerRegistry::create(
          prod_cfg.type,
          camera_components.at(prod_cfg.hardware_id),
          prod_cfg.to_registry_json(cam->width, cam->height, cam->fps));
        mgr.add_push_producer(prod);
        std::cout << "  [ok] Camera producer (push) ["
                  << prod_cfg.stream_id << "] ("
                  << cam->width << "x" << cam->height << ")\n";
      } else {
        auto prod = trossen::runtime::ProducerRegistry::create(
          prod_cfg.type,
          camera_components.at(prod_cfg.hardware_id),
          prod_cfg.to_registry_json(cam->width, cam->height, cam->fps));
        mgr.add_producer(prod, period);
        std::cout << "  [ok] Camera producer ["
                  << prod_cfg.stream_id << "] ("
                  << prod_cfg.poll_rate_hz << " Hz, "
                  << cam->width << "x" << cam->height << ")\n";
      }
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
    // Send the arms back to their configured staging poses so the
    // operator has a known-safe starting point for the next episode.
    // wait_for_reset() then blocks (infinitely, when reset_duration
    // is unset) while the operator repositions the VR controller;
    // the next prepare_teleop() re-syncs so the new VR pose becomes
    // the anchor.
    for (auto& ctrl : controllers) ctrl->restage();
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

  // ── Attach session-control source ───────────────────────────────────────
  //
  // With a VrSessionControlComponent attached, A/B/grip buttons drive the
  // SessionManager loops directly. The initial `wait_for_reset()` doubles
  // as the pre-session gate — operator puts on the headset, presses A,
  // recording begins.

  if (session_control) {
    mgr.attach_session_control(session_control);
    std::cout << "\nPut on the Meta Quest and press A on the controller to "
                 "start recording.\n"
                 "  A    = start / skip-reset / stop-current-and-advance\n"
                 "  B    = re-record current or last episode\n"
                 "  grip = end session\n\n";
  } else {
    std::cout << "\n(No VR session-control configured — using keyboard: "
                 "-> continue, <- re-record, Ctrl+C to end.)\n\n";
  }

  {
    const auto initial = mgr.wait_for_reset();
    if (initial == trossen::runtime::UserAction::kStop ||
        trossen::utils::g_stop_requested) {
      std::cout << "\nAborted before first episode.\n";
      mgr.detach_session_control();
      mgr.shutdown();
      return 0;
    }
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

    // Keep the slate base producer warm for the duration of the episode.
    // The session manager schedules poll() at `base_rate_hz`; this extra
    // 10 Hz poke reads-and-discards so the underlying driver sees frequent
    // traffic even during the scheduler's idle gaps.
    std::thread velocity_thread([&]() {
      if (!base_prod) return;
      while (mgr.is_episode_active() && !trossen::utils::g_stop_requested) {
        base_prod->poll([](std::shared_ptr<trossen::data::RecordBase>) {});
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });

    auto action = mgr.monitor_episode();
    if (action == trossen::runtime::UserAction::kReRecord) {
      mgr.discard_current_episode();
      if (velocity_thread.joinable()) velocity_thread.join();
      continue;
    }

    if (mgr.is_episode_active()) mgr.stop_episode();
    if (velocity_thread.joinable()) velocity_thread.join();

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

  mgr.detach_session_control();
  mgr.shutdown();

  const auto final_stats = mgr.stats();
  std::vector<std::string> extra_info = {
    "Data streams:         " +
      std::to_string(cfg.hardware.arms.size()) + " follower arm(s)" +
      (slate_base_component ? " + mobile base" : "") + ", " +
      std::to_string(vr_arm_components.size())  + " VR arm leader(s), " +
      std::to_string(vr_base_components.size()) + " VR base joystick(s)"
  };
  trossen::utils::print_final_summary(
    final_stats.total_episodes_completed, root, extra_info);

  return 0;
}
