/**
 * @file trossen_adamo_remote_follower.cpp
 * @brief Remote-teleop follower: drive a local follower arm from a leader whose
 *        joint state is routed through the Adamo cloud. EXPERIMENTAL.
 *
 * Pairs with a leader host running `trossen_solo_ai` + an AdamoObserver that
 * publishes the physical leader's joint state to `<robot>/<arm>/state`. This
 * binary constructs a virtual `remote_adamo_leader` arm that subscribes to that
 * topic and mirrors it onto the local follower via the standard
 * TeleopController, so no local leader hardware is required.
 *
 * The virtual leader does NOT fit the `hardware.arms` / ArmConfig schema (its
 * config is robot/arm/protocol/..., not ip/model/end_effector), so it lives in
 * a separate top-level `remote_leader_hardware` block that the shared SdkConfig
 * parser ignores. We construct each entry from raw JSON via
 * HardwareRegistry::create(), which configures it and registers it in
 * ActiveHardwareRegistry under its id, so the teleop factory resolves it by name
 * in a pair's "leader" field.
 *
 * Requires `-DTROSSEN_ENABLE_ADAMO=ON` (the `remote_adamo_leader` hardware type
 * is only compiled into trossen_sdk under that gate) and `ADAMO_API_KEY` set.
 *
 * Usage:
 *   ./trossen_adamo_remote_follower [OPTIONS]
 *
 *   --config PATH       Path to follower config JSON
 *                       [default: examples/trossen_adamo_remote/follower.config.json]
 *   --set KEY=VALUE     Override a config value using dot notation (repeatable)
 *   --dump-config       Print merged config as JSON and exit
 *   --help              Show this help and exit
 *
 * Startup order + safety (see examples/trossen_adamo_remote/README.md):
 *   1. Start the leader publisher FIRST. The virtual leader's
 *      prepare_for_teleop() blocks up to `ready_timeout_s` for the first frame.
 *   2. Start this follower while the physical leader is near the follower's
 *      home/staged pose, THEN move the leader — otherwise the follower snaps to
 *      the leader's pose on the first real frame.
 */

#include <chrono>
#include <cstdlib>
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
#ifdef TROSSEN_ENABLE_ADAMO
#include "trossen_sdk/observer/adamo_observer.hpp"
#endif
#include "trossen_sdk/hw/teleop/teleop_factory.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"
#include "trossen_sdk/runtime/push_producer_registry.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"

#include "trossen_sdk/utils/app_utils.hpp"

static void print_usage(const char* program) {
  std::cout <<
    "Usage: " << program << " [OPTIONS]\n"
    "\n"
    "Drive a local follower arm from a remote leader routed through Adamo.\n"
    "\n"
    "Options:\n"
    "  --config PATH      Path to follower config JSON\n"
    "                     [default: examples/trossen_adamo_remote/follower.config.json]\n"
    "  --set KEY=VALUE    Override a config value using dot notation (repeatable)\n"
    "  --dump-config      Print merged config as JSON and exit\n"
    "  --help             Show this help and exit\n"
    "\n"
    "Requires -DTROSSEN_ENABLE_ADAMO=ON and ADAMO_API_KEY in the environment.\n"
    "Start the leader publisher BEFORE this follower.\n"
    "\n"
    "Examples:\n"
    "  " << program << "\n"
    "  " << program << " --config examples/trossen_adamo_remote/follower.config.json\n"
    "  " << program << " --set hardware.arms.follower.ip_address=192.168.1.3\n"
    "  " << program << " --dump-config\n";
}

int main(int argc, char** argv) {
  trossen::configuration::CliParser cli(argc, argv);

  if (cli.has_flag("help")) {
    print_usage(argv[0]);
    return 0;
  }

  const std::string config_path =
    cli.get_string("config", "examples/trossen_adamo_remote/follower.config.json");

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
    trossen::configuration::dump_config(j, "Trossen Adamo Remote Follower Config");
    return 0;
  }

  // Parse unified robot config. The "remote_leader_hardware" block is not part
  // of SdkConfig; we read it straight off `j` below.
  auto cfg = trossen::configuration::SdkConfig::from_json(j);

  // Populate GlobalConfig so SessionManager + teleop factory pick up session,
  // backend, and teleop settings.
  cfg.populate_global_config();

  const std::string root = cfg.mcap_backend.root;

  // Derive joint rate from the producer list (for the post-episode sanity check)
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
  if (j.contains("remote_leader_hardware") && j.at("remote_leader_hardware").is_object()) {
    for (const auto& [id, entry] : j.at("remote_leader_hardware").items()) {
      const std::string type =
        (entry.is_object() && entry.contains("type") && entry.at("type").is_string())
          ? entry.at("type").get<std::string>() : std::string{"?"};
      config_lines.push_back("Virtual leader [" + id + "]:  type=" + type);
    }
  }
  config_lines.push_back("Joint rate:           " + std::to_string(joint_rate_hz) + " Hz");
  config_lines.push_back(
    "Teleop:               " +
    std::string(cfg.teleop.enabled ? "enabled" : "disabled") +
    " (" + std::to_string(cfg.teleop.pairs.size()) + " pairs)");

  trossen::utils::print_config_banner(
    "Trossen Adamo Remote Follower (EXPERIMENTAL)", config_lines);

  trossen::utils::install_signal_handler();
  std::filesystem::create_directories(root);

  // ──────────────────────────────────────────────────────────
  // Initialize physical arm hardware from config (the local follower)
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
  // Virtual leaders fed by remote Adamo pubsub.
  //
  // HardwareRegistry::create() configures the component and registers it in
  // ActiveHardwareRegistry under `id`, so create_controllers_from_global_config()
  // can resolve it as a pair's "leader". Construction failure is fatal (the
  // whole point of this binary is the remote leader) but we `return 1` rather
  // than throw, so `mgr` (declared below in solo's order) is never reached and
  // the already-created follower's destructor still runs.
  // ──────────────────────────────────────────────────────────

  if (j.contains("remote_leader_hardware")) {
    const auto& block = j.at("remote_leader_hardware");
    if (!block.is_object()) {
      std::cerr << "Error: 'remote_leader_hardware' must be a JSON object.\n";
      return 1;
    }
    std::cout << "Creating remote (virtual) leaders...\n";
    for (const auto& [id, entry] : block.items()) {
      if (!entry.is_object() || !entry.contains("type") ||
          !entry.at("type").is_string()) {
        std::cerr << "  [err] remote_leader_hardware[" << id
                  << "] needs a string 'type' field.\n";
        return 1;
      }
      const std::string type = entry.at("type").get<std::string>();
      try {
        trossen::hw::HardwareRegistry::create(type, id, entry, /*mark_active=*/true);
        std::cout << "  [ok] Virtual leader [" << id << "] type=" << type << "\n";
      } catch (const std::exception& e) {
        std::cerr << "  [err] Virtual leader [" << id << "] type=" << type
                  << " failed to construct: " << e.what() << "\n";
        if (!trossen::hw::HardwareRegistry::is_registered(type)) {
          std::cerr << "        Hint: type '" << type << "' is not registered. "
                    << "Rebuild with -DTROSSEN_ENABLE_ADAMO=ON.\n";
        }
        return 1;
      }
    }
  } else {
    std::cerr << "Warning: no 'remote_leader_hardware' block in config; the teleop "
              << "pair's leader will not resolve and teleop will be skipped.\n";
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

  // Pre-initialize camera hardware (keyed by camera id for producer lookup).
  // Any camera configured here is published to Adamo by the AdamoObserver as a
  // video track, so a remote operator can see the follower's scene on the web UI.
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

  // One producer per entry in the producers list (the local follower arm, plus
  // any cameras streamed to Adamo).
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

  std::cout << "\nProducers registered. Ready to teleop.\n";

  // ──────────────────────────────────────────────────────────
  // Observers: registered once, started lazily on first episode, persist across
  // episodes. The follower may publish its effort back onto Adamo.
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
        // Observers are non-essential by design: a missing/misconfigured one logs
        // and is skipped so teleop continues without the stream.
        std::cerr << "  [skip] Observer [" << obs_cfg.id << "] type=" << obs_cfg.type
                  << " failed to construct: " << e.what() << "\n";
        const auto types =
          trossen::observer::ObserverRegistry::get_registered_types();
        std::cerr << "         Registered observer types:";
        for (const auto& t : types) std::cerr << " " << t;
        std::cerr << "\n";
        if (!trossen::observer::ObserverRegistry::is_registered(obs_cfg.type)) {
          std::cerr << "         Hint: type '" << obs_cfg.type
                    << "' is not registered. Rebuild with -DTROSSEN_ENABLE_ADAMO=ON "
                    << "(for 'adamo') or set 'enabled': false on this observer.\n";
        }
        continue;
      }
    }
  }

  // ──────────────────────────────────────────────────────────
  // Teleop controllers (constructor stages arms). The virtual leader is
  // resolved from ActiveHardwareRegistry by the name in the pair's "leader".
  // ──────────────────────────────────────────────────────────

  auto controllers = trossen::hw::teleop::create_controllers_from_global_config();
  if (controllers.empty()) {
    std::cerr << "Warning: no teleop controllers were created. Check that the pair's "
              << "leader matches a 'remote_leader_hardware' id and the follower matches "
              << "a 'hardware.arms' id.\n";
  }

  // Camera rate + depth counts for the post-episode sanity check.
  float camera_fps = 30.0f;
  for (const auto& p : cfg.producers) {
    if (p.type == "realsense_camera" || p.type == "zed_camera" ||
        p.type == "opencv_camera") {
      camera_fps = p.poll_rate_hz;
      break;
    }
  }
  int depth_cameras = 0;
  for (const auto& cam : cfg.hardware.cameras) {
    if (cam.use_depth) ++depth_cameras;
  }

  // ──────────────────────────────────────────────────────────
  // Register lifecycle callbacks
  // ──────────────────────────────────────────────────────────

  // Before each episode: controllers open the Adamo session and block for the
  // first leader frame (readiness), then prepare both arms.
  mgr.on_pre_episode([&]() -> bool {
    for (auto& ctrl : controllers) ctrl->prepare_teleop();
    return true;
  });

  // Episode started: begin mirroring (alongside recording)
  mgr.on_episode_started([&]() {
    for (auto& ctrl : controllers) ctrl->teleop();
    std::cout << "Episode started - recording"
              << (controllers.empty() ? "" : " and mirroring")
              << " active.\n";
  });

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

  // Shutdown: stop mirror + return arms to rest. The virtual leader's
  // end_teleop() (invoked by stop_teleop) drains the subscriber + closes the
  // Adamo session; the follower returns to rest.
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

    std::cout << "Mirroring remote leader...\n";

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
    "Data streams:         " +
      std::to_string(cfg.hardware.arms.size()) + " follower arm(s)"
  };
  trossen::utils::print_final_summary(
    final_stats.total_episodes_completed, root, extra_info);

  // If a camera was streamed, the AdamoObserver opened a libadamo Robot video
  // pipeline whose run-loop thread cannot be stopped (no SDK hook) and is detached
  // at observer teardown. Letting C++ global destructors run would deadlock against
  // that live thread. Everything that matters (arm parking in on_pre_shutdown, Adamo
  // session close) already happened in mgr.shutdown() above, so hard-exit to skip the
  // hanging teardown. Joint-only runs (no camera) fall through to a normal return.
#ifdef TROSSEN_ENABLE_ADAMO
  if (trossen::observer::AdamoObserver::video_pipeline_active()) {
    std::cout << "[adamo] camera pipeline cannot be stopped cleanly; "
                 "hard-exiting to avoid teardown deadlock\n";
    std::cout.flush();
    std::fflush(nullptr);
    std::_Exit(0);
  }
#endif

  return 0;
}
