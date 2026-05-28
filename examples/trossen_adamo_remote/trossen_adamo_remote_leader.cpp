/**
 * @file trossen_adamo_remote_leader.cpp
 * @brief Remote-teleop leader (direct publish). EXPERIMENTAL.
 *
 * A minimal leader publisher that talks to the Adamo server directly: no
 * SessionManager, no observer, no producer registry. It owns the leader arm(s),
 * drives them into gravity-compensation, and runs a tight
 * read -> wire::encode_state -> publisher.put loop, mirroring upstream
 * trossen_adamo's leader binary.
 *
 * Use this when you only need to stream leader state (no recording, no episode
 * lifecycle). There is no episode gating: Ctrl+C stops the loop immediately.
 *
 * Config: uses leader.config.json. The arms come from `hardware.arms`; the Adamo
 * connection params (robot / protocol / api_key_env) are read from the `adamo`
 * observer block in that config. Every arm in `hardware.arms` is published to
 * `<robot>/<arm>/state` at the trossen_arm producer's poll_rate_hz.
 *
 * Requires -DTROSSEN_ENABLE_ADAMO=ON and ADAMO_API_KEY set.
 */

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"
#include "nlohmann/json.hpp"

#include "adamo/adamo.hpp"
#include "trossen_adamo/args.hpp"
#include "trossen_adamo/publisher.hpp"
#include "trossen_adamo/wire.hpp"

#include "adamo_lifecycle.hpp"

#include "trossen_sdk/configuration/cli_parser.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/configuration/sdk_config.hpp"
#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/utils/app_utils.hpp"

namespace {

constexpr std::size_t kNumJoints = trossen_adamo::wire::kNumJoints;  // 7

void print_usage(const char* program) {
  std::cout <<
    "Usage: " << program << " [OPTIONS]\n"
    "\n"
    "Direct-publish remote-teleop leader: streams leader joint state straight to\n"
    "Adamo (no SessionManager / observer). Reuses leader.config.json.\n"
    "\n"
    "Options:\n"
    "  --config PATH      Path to leader config JSON\n"
    "                     [default: examples/trossen_adamo_remote/leader.config.json]\n"
    "  --set KEY=VALUE    Override a config value using dot notation (repeatable)\n"
    "  --dump-config      Print merged config as JSON and exit\n"
    "  --help             Show this help and exit\n"
    "\n"
    "Requires -DTROSSEN_ENABLE_ADAMO=ON and ADAMO_API_KEY in the environment.\n";
}

}  // namespace

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

  auto j = trossen::configuration::JsonLoader::load(config_path);
  const auto overrides = cli.get_set_overrides();
  if (!overrides.empty()) {
    j = trossen::configuration::merge_overrides(j, overrides);
  }
  if (cli.has_flag("dump-config")) {
    trossen::configuration::dump_config(j, "Trossen Adamo Remote Leader (direct) Config");
    return 0;
  }

  auto cfg = trossen::configuration::SdkConfig::from_json(j);
  cfg.populate_global_config();

  // ── Adamo connection params, read from the `adamo` observer block so this
  //    binary and the observer-based leader share one config file. ───────────
  const nlohmann::json* adamo = nullptr;
  for (const auto& o : cfg.observers) {
    if (o.type == "adamo") {
      adamo = &o.raw_json;
      break;
    }
  }
  if (adamo == nullptr) {
    std::cerr << "Error: leader config has no 'adamo' observer block; this binary "
              << "reads robot/protocol/api_key_env from it.\n";
    return 1;
  }
  if (!adamo->contains("robot") || !adamo->at("robot").is_string()) {
    std::cerr << "Error: the 'adamo' block needs a string 'robot' (topic prefix).\n";
    return 1;
  }
  const std::string robot       = adamo->at("robot").get<std::string>();
  const std::string protocol    = adamo->value("protocol", std::string{"quic"});
  const std::string api_key_env = adamo->value("api_key_env", std::string{"ADAMO_API_KEY"});

  const char* api_key = std::getenv(api_key_env.c_str());
  if (api_key == nullptr || std::strlen(api_key) == 0) {
    std::cerr << "Error: " << api_key_env << " is unset; cannot open Adamo session.\n";
    return 1;
  }

  adamo::Protocol proto;
  try {
    proto = trossen_adamo::args::parse_protocol(protocol);
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  // Publish rate from the first trossen_arm producer (default 100 Hz).
  double rate_hz = 100.0;
  for (const auto& p : cfg.producers) {
    if (p.type == "trossen_arm") {
      rate_hz = p.poll_rate_hz;
      break;
    }
  }
  if (!(rate_hz > 0.0)) rate_hz = 100.0;

  // Lifecycle coordination (opt-in): publish READY heartbeat so the follower can
  // wait for us, and terminate when the follower signals STOPPING.
  const bool lifecycle_enabled =
    j.value("lifecycle", nlohmann::json::object()).value("enabled", false);

  std::vector<std::string> lines = {
    "Config file:          " + config_path,
    "Robot prefix:         " + robot,
    "Protocol:             " + protocol,
    "Publish rate:         " + std::to_string(rate_hz) + " Hz",
    std::string("Lifecycle link:       ") + (lifecycle_enabled ? "on" : "off"),
  };
  for (const auto& [id, arm] : cfg.hardware.arms) {
    lines.push_back("Arm [" + id + "]:  " + arm.ip_address + " -> " + robot + "/" + id + "/state");
  }
  trossen::utils::print_config_banner(
    "Trossen Adamo Remote Leader (direct publish, EXPERIMENTAL)", lines);

  trossen::utils::install_signal_handler();

  // ── Create + engage the leader arms ───────────────────────────────────────
  struct Pub {
    std::string id;
    std::shared_ptr<trossen::hw::arm::TrossenArmComponent> comp;
    std::shared_ptr<trossen_arm::TrossenArmDriver> driver;
    std::unique_ptr<trossen_adamo::LatestPublisher> latest;
  };
  std::vector<Pub> pubs;

  std::cout << "Initializing leader arms...\n";
  for (const auto& [id, arm_cfg] : cfg.hardware.arms) {
    // mark_active=false: no teleop factory / ActiveHardwareRegistry needed here.
    auto component = trossen::hw::HardwareRegistry::create(
      "trossen_arm", id, arm_cfg.to_json(), /*mark_active=*/false);
    auto comp = std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(component);
    if (!comp) {
      std::cerr << "  [skip] " << id << ": not a TrossenArmComponent\n";
      continue;
    }
    auto driver = comp->get_hardware();
    if (!driver) {
      std::cerr << "  [skip] " << id << ": null driver\n";
      continue;
    }
    if (static_cast<std::size_t>(driver->get_num_joints()) != kNumJoints) {
      std::cerr << "  [skip] " << id << ": " << driver->get_num_joints()
                << " joints (wire codec is " << kNumJoints << "-DOF only)\n";
      continue;
    }
    pubs.push_back(Pub{id, comp, driver, nullptr});
    std::cout << "  [ok] Arm [" << id << "] configured (" << arm_cfg.ip_address << ")\n";
  }
  if (pubs.empty()) {
    std::cerr << "Error: no publishable 7-DOF leader arms; nothing to do.\n";
    return 1;
  }

  // Gravity-comp so the operator can back-drive the leaders.
  for (auto& p : pubs) p.comp->prepare_for_teleop();
  std::cout << "Leader arms in gravity-comp — back-drive them.\n";

  // ── Open session + one publisher per arm ──────────────────────────────────
  std::unique_ptr<adamo::Session> session;
  try {
    session = std::make_unique<adamo::Session>(
      adamo::Session::open(std::string(api_key), proto));
    for (auto& p : pubs) {
      const std::string topic = robot + "/" + p.id + "/state";
      // Real-time joint state: high priority, express path, fire-and-forget.
      // Mirrors upstream trossen_adamo/leader.cpp; the default
      // (priority=4, express=false, reliable=true) forces Adamo's reliable
      // layer to retransmit + sequence, which produces ~50 ms wire stalls
      // on packet loss. Latest-wins joint state is fine to drop.
      p.latest = std::make_unique<trossen_adamo::LatestPublisher>(
        session->publisher(topic, /*priority=*/250,
                           /*express=*/true, /*reliable=*/false));
    }
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to open Adamo session/publishers: " << e.what() << "\n";
    for (auto& p : pubs) {
      try { p.comp->end_teleop(); } catch (...) {}
    }
    return 1;
  }

  // Optional lifecycle channel on the same session.
  std::unique_ptr<trossen_adamo_remote::LifecycleLink> life;
  if (lifecycle_enabled) {
    try {
      life = std::make_unique<trossen_adamo_remote::LifecycleLink>(
        *session,
        trossen_adamo_remote::leader_lifecycle_topic(robot),
        trossen_adamo_remote::follower_lifecycle_topic(robot));
      life->announce(trossen_adamo_remote::LifeState::kReady);
      std::cout << "[lifecycle] announcing READY; will terminate when the follower stops.\n";
    } catch (const std::exception& e) {
      std::cerr << "[lifecycle] failed to open channel: " << e.what()
                << " (continuing without coordination)\n";
      life.reset();
    }
  }

  std::cout << "Publishing leader state to Adamo. Move the leader arm(s). Ctrl+C to stop.\n";

  // ── Direct publish loop (no episode lifecycle) ────────────────────────────
  const auto period = std::chrono::nanoseconds(static_cast<std::int64_t>(1e9 / rate_hz));
  auto last_heartbeat = std::chrono::steady_clock::now();
  std::uint64_t ticks = 0;
  std::uint64_t skipped = 0;
  while (!trossen::utils::g_stop_requested) {
    const auto deadline = std::chrono::steady_clock::now() + period;
    const double ts = trossen_adamo::wire::now_seconds();
    for (auto& p : pubs) {
      // get_robot_output() returns the daemon's latest joint snapshot
      // (positions + velocities in one read), so no second daemon round-trip.
      auto out = p.driver->get_robot_output();
      const auto& positions  = out.joint.all.positions;
      const auto& velocities = out.joint.all.velocities;
      if (positions.size() != kNumJoints || velocities.size() != kNumJoints) {
        ++skipped;
        continue;
      }
      try {
        const auto buf = trossen_adamo::wire::encode_state(ts, positions, velocities);
        p.latest->put(buf.data(), buf.size());
      } catch (const std::exception& e) {
        ++skipped;
        std::cerr << "[" << p.id << "] encode/publish failed: " << e.what() << "\n";
      }
    }
    if (life) {
      const auto now = std::chrono::steady_clock::now();
      if (now - last_heartbeat >= std::chrono::milliseconds(500)) {
        life->announce(trossen_adamo_remote::LifeState::kReady);
        last_heartbeat = now;
      }
      if (life->poll_peer() == trossen_adamo_remote::LifeState::kStopping) {
        std::cout << "\n[lifecycle] follower ended its session; terminating leader.\n";
        break;
      }
    }
    ++ticks;
    std::this_thread::sleep_until(deadline);
  }

  std::cout << "\nStopping (Ctrl+C). Published " << ticks << " ticks";
  if (skipped > 0) std::cout << " (" << skipped << " frames skipped)";
  std::cout << ".\n";

  // ── Shutdown: tell the follower we're stopping, stop publishing (joins worker
  //    threads) before closing the session, then return the arms to rest. ─────
  if (life) {
    life->announce(trossen_adamo_remote::LifeState::kStopping);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // flush before close
  }
  for (auto& p : pubs) p.latest.reset();
  life.reset();   // release the lifecycle pub/sub before the session it holds onto
  session.reset();
  for (auto& p : pubs) {
    try {
      p.comp->end_teleop();
    } catch (const std::exception& e) {
      std::cerr << "[" << p.id << "] end_teleop failed: " << e.what() << "\n";
    }
  }
  std::cout << "Leader arms returned to rest. Done.\n";
  return 0;
}
