/**
 * @file replay_trossen_mcap_jointstate.cpp
 * @brief Replay joint states from a TrossenMCAP file to robot arms
 *
 * Reads joint state data from an MCAP file and replays it on the configured
 * robot arms. Stream IDs in the recording are matched to the configured arms.
 * Recorded base odometry streams replay their velocities onto a SLATE base, or,
 * in a build with TROSSEN_ENABLE_RIVET, onto a Rivet swerve base and its lift.
 *
 * Usage:
 *   ./replay_trossen_mcap_jointstate <path_to_mcap_file> [options]
 *
 * Options:
 *   --config <path>   Config JSON file
 *                     (default: scripts/replay_trossen_mcap_jointstate/config.json)
 *   --set KEY=VALUE   Override a config value (repeatable)
 *   --speed <float>   Playback speed multiplier (default: from config)
 *   --help            Show this help message
 *
 * Examples:
 *   ./replay_trossen_mcap_jointstate ~/datasets/0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap
 *   ./replay_trossen_mcap_jointstate ~/datasets/episode.mcap --speed 0.5
 *   ./replay_trossen_mcap_jointstate ~/datasets/episode.mcap --config my_config.json
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"
#include "mcap/reader.hpp"
#include "nlohmann/json.hpp"
#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/base/slate_base_component.hpp"
#ifdef TROSSEN_ENABLE_RIVET
#include "trossen_sdk/hw/base/trossen_base_component.hpp"
#endif
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/configuration/cli_parser.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"

#include "JointState.pb.h"
#include "Odometry2D.pb.h"
#include "trossen_sdk/utils/app_utils.hpp"

struct ArmConfig {
  std::string stream_id;
  std::string ip_address;
  std::string model;
  std::string end_effector;
  // Duration passed to set_all_positions(). Set to 2.0/fps (e.g. 0.066 for 30 Hz) for smooth
  // chained motion — two frame periods gives the controller enough headroom to interpolate
  // cleanly between commands. 0.0 commands an immediate step which produces jerky replay.
  double goal_time = 0.0;
};

struct SlateConfig {
  std::string stream_id;
  bool reset_odometry = false;
  bool enable_torque = true;
};

/// A Rivet swerve base. Every key other than `stream_id` is passed to
/// TrossenBaseComponent::configure() unchanged, so the speed limits, homing and
/// command timeout behave exactly as they do when recording.
struct TrossenBaseConfig {
  std::string stream_id;
  nlohmann::json hardware;
};

struct ReplayConfig {
  std::string mcap_file;
  std::vector<ArmConfig> arms;
  std::vector<SlateConfig> slates;
  std::vector<TrossenBaseConfig> trossen_bases;
  float playback_speed = 1.0f;
};

static ReplayConfig load_replay_config(const nlohmann::json& j) {
  ReplayConfig cfg;
  const auto& r = j.at("replay");

  if (r.contains("playback_speed")) {
    r.at("playback_speed").get_to(cfg.playback_speed);
  }

  if (r.contains("arms")) {
    for (const auto& arm : r.at("arms")) {
      ArmConfig a;
      arm.at("stream_id").get_to(a.stream_id);
      arm.at("ip_address").get_to(a.ip_address);
      arm.at("model").get_to(a.model);
      arm.at("end_effector").get_to(a.end_effector);
      if (arm.contains("goal_time")) arm.at("goal_time").get_to(a.goal_time);
      cfg.arms.push_back(a);
    }
  }

  if (r.contains("slates")) {
    for (const auto& slate : r.at("slates")) {
      SlateConfig s;
      slate.at("stream_id").get_to(s.stream_id);
      if (slate.contains("reset_odometry")) slate.at("reset_odometry").get_to(s.reset_odometry);
      if (slate.contains("enable_torque")) slate.at("enable_torque").get_to(s.enable_torque);
      cfg.slates.push_back(s);
    }
  }

  if (r.contains("trossen_bases")) {
    for (const auto& base : r.at("trossen_bases")) {
      TrossenBaseConfig b;
      base.at("stream_id").get_to(b.stream_id);
      b.hardware = base;
      b.hardware.erase("stream_id");
      cfg.trossen_bases.push_back(b);
    }
  }

  return cfg;
}

int main(int argc, char** argv) {
  namespace fs = std::filesystem;

  trossen::configuration::CliParser cli(argc, argv);

  if (cli.has_flag("help")) {
    std::cerr << "Usage: " << argv[0] << " <path_to_mcap_file> [options]\n";
    std::cerr << "\nOptions:\n";
    std::cerr << "  --config <path>   Config JSON "
              << "(default: scripts/replay_trossen_mcap_jointstate/config.json)\n";
    std::cerr << "  --set KEY=VALUE   Override config value (repeatable)\n";
    std::cerr << "  --speed <float>   Playback speed multiplier\n";
    std::cerr << "  --help            Show this help\n";
    std::cerr << "\nExamples:\n";
    std::cerr << "  " << argv[0] << " ~/datasets/0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap\n";
    std::cerr << "  " << argv[0] << " ~/datasets/episode.mcap --speed 0.5\n";
    std::cerr << "  " << argv[0] << " ~/datasets/episode.mcap --config my_config.json\n";
    return 0;
  }

  const auto& pos_args = cli.get_positional();
  if (pos_args.empty()) {
    std::cerr << "Usage: " << argv[0] << " <path_to_mcap_file> [options]\n";
    std::cerr << "Run with --help for full usage.\n";
    return 1;
  }

  // Load config
  const std::string config_path =
      cli.get_string("config", "scripts/replay_trossen_mcap_jointstate/config.json");
  if (!fs::exists(config_path)) {
    std::cerr << "Error: config file not found: " << config_path << "\n";
    std::cerr << "Run from the repository root or use --config <path>.\n";
    return 1;
  }

  auto j = trossen::configuration::JsonLoader::load(config_path);
  const auto overrides = cli.get_set_overrides();
  if (!overrides.empty()) {
    j = trossen::configuration::merge_overrides(j, overrides);
  }

  ReplayConfig cfg = load_replay_config(j);
  cfg.mcap_file = pos_args[0];

  // --speed flag overrides config playback_speed
  if (cli.has_flag("speed")) {
    cfg.playback_speed = cli.get_float("speed", cfg.playback_speed);
  }
  // Above 2x the arms are sent targets faster than they can track, and a very large
  // value makes every sample due at once, which jumps each arm to its last pose.
  constexpr float kMaxPlaybackSpeed = 2.0f;
  if (!(cfg.playback_speed > 0.0f) || !std::isfinite(cfg.playback_speed) ||
      cfg.playback_speed > kMaxPlaybackSpeed) {
    std::cerr << "Error: playback speed must be above 0 and at most " << kMaxPlaybackSpeed
              << ", got " << cfg.playback_speed << "\n";
    return 1;
  }

  // Check if MCAP file exists
  if (!fs::exists(cfg.mcap_file)) {
    std::cerr << "Error: MCAP file not found: " << cfg.mcap_file << "\n";
    return 1;
  }

  // Print configuration
  std::vector<std::string> config_lines = {
    "MCAP file:        " + cfg.mcap_file,
    "Config:           " + config_path,
    "Playback speed:   " + std::to_string(cfg.playback_speed) + "x",
    "Arms configured:  " + std::to_string(cfg.arms.size())
  };
  for (const auto& arm : cfg.arms) {
    config_lines.push_back("  - " + arm.stream_id + " (" + arm.ip_address + ")");
  }

  trossen::utils::print_config_banner("MCAP Joint State Replay", config_lines);
  trossen::utils::install_signal_handler();
  // A replay run over ssh dies on SIGHUP when the session drops, and SIGTERM is what
  // a supervisor sends. Both take the same path as Ctrl+C, so the base is stopped and
  // the arms returned to rest instead of the process ending with the base moving.
  std::signal(SIGTERM, [](int) { trossen::utils::g_stop_requested = true; });
  std::signal(SIGHUP, [](int) { trossen::utils::g_stop_requested = true; });

  // ──────────────────────────────────────────────────────────
  // Initialize hardware
  // ──────────────────────────────────────────────────────────

  std::map<std::string, std::shared_ptr<trossen_arm::TrossenArmDriver>> drivers;
  std::map<std::string, double> driver_goal_times;
  std::map<std::string, std::shared_ptr<trossen::hw::HardwareComponent>> components;
  std::map<std::string, std::shared_ptr<trossen_slate::TrossenSlate>> slate_drivers;
#ifdef TROSSEN_ENABLE_RIVET
  std::map<std::string, std::shared_ptr<trossen::hw::base::TrossenBaseComponent>> trossen_bases;
#endif

  std::cout << "Initializing hardware...\n";

  for (const auto& arm_cfg : cfg.arms) {
    nlohmann::json hw_cfg = {
      {"ip_address", arm_cfg.ip_address},
      {"model", arm_cfg.model},
      {"end_effector", arm_cfg.end_effector}
    };

    auto component = trossen::hw::HardwareRegistry::create(
      "trossen_arm", arm_cfg.stream_id, hw_cfg, true);

    auto arm_component = std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(
      component);

    if (!arm_component) {
      std::cerr << "  [FAILED] Failed to create component for " << arm_cfg.stream_id << "\n";
      continue;
    }

    auto driver = arm_component->get_hardware();
    if (!driver) {
      std::cerr << "  [FAILED] Failed to get driver for " << arm_cfg.stream_id << "\n";
      continue;
    }

    drivers[arm_cfg.stream_id] = driver;
    driver_goal_times[arm_cfg.stream_id] = arm_cfg.goal_time;
    components[arm_cfg.stream_id] = component;
    std::cout << "  [ok] " << arm_cfg.stream_id << " initialized (" << arm_cfg.ip_address << ")\n";
  }

  if (drivers.empty()) {
    std::cerr << "Error: No arms initialized successfully\n";
    return 1;
  }

  // Set all arms to position mode
  std::cout << "\nSetting arms to position control mode...\n";
  for (auto& [stream_id, driver] : drivers) {
    driver->set_all_modes(trossen_arm::Mode::position);
  }

  // Time for the move to the recorded starting positions.
  const float moving_time_s = 2.0f;

  // ──────────────────────────────────────────────────────────
  // Read MCAP file and parse joint states
  // ──────────────────────────────────────────────────────────

  std::cout << "\nReading MCAP file: " << cfg.mcap_file << "\n";

  std::ifstream input(cfg.mcap_file, std::ios::binary);
  if (!input.is_open()) {
    std::cerr << "Error: Failed to open MCAP file\n";
    return 1;
  }

  mcap::McapReader reader;
  auto status = reader.open(input);
  if (!status.ok()) {
    std::cerr << "Error: Failed to parse MCAP file: " << status.message << "\n";
    return 1;
  }

  auto summary_status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
  if (!summary_status.ok()) {
    std::cerr << "Error: Failed to read MCAP summary: " << summary_status.message << "\n";
    return 1;
  }

  struct JointStateMessage {
    // Monotonic capture time when the recording has it, else the MCAP log time.
    uint64_t timestamp_ns;
    // MCAP log time, kept for the fallback.
    uint64_t log_time_ns;
    std::vector<double> positions;
    std::vector<double> velocities;
    // Base streams only: the recorded lift velocity.
    double lift_velocity = 0.0;
  };

  std::map<std::string, std::vector<JointStateMessage>> messages_by_stream;
  std::map<mcap::ChannelId, std::string> channel_id_to_stream;

  std::cout << "  Available channels in MCAP file:\n";
  for (const auto& [channel_id, channel_ptr] : reader.channels()) {
    std::cout << "    - Topic: '" << channel_ptr->topic << "' (Schema: "
              << channel_ptr->schemaId << ", ID: " << channel_id << ")\n";
  }

  // Detect arm joint state channels ({stream_id}/joints/state)
  // and base odometry channels ({stream_id}/odom/state) separately.
  std::map<mcap::ChannelId, std::string> odom_channel_id_to_stream;
  std::set<std::string> base_stream_ids;

  for (const auto& [channel_id, channel_ptr] : reader.channels()) {
    const std::string& topic = channel_ptr->topic;

    size_t pos = topic.find("/joints/state");
    if (pos != std::string::npos) {
      std::string stream_id = topic.substr(0, pos);
      channel_id_to_stream[channel_id] = stream_id;
      std::cout << "  Found joint state channel: " << topic
                << " (stream_id: " << stream_id << ")\n";
      continue;
    }

    pos = topic.find("/odom/state");
    if (pos != std::string::npos) {
      std::string stream_id = topic.substr(0, pos);
      odom_channel_id_to_stream[channel_id] = stream_id;
      base_stream_ids.insert(stream_id);
      std::cout << "  Found odometry channel:   " << topic
                << " (stream_id: " << stream_id << ")\n";
    }
  }

  if (channel_id_to_stream.empty() && odom_channel_id_to_stream.empty()) {
    std::cerr << "Error: No replayable channels found in MCAP file\n";
    std::cerr << "Expected topics: '{stream_id}/joints/state' or '{stream_id}/odom/state'\n";
    return 1;
  }

  std::cout << "\nParsing messages...\n";
  size_t total_messages = 0;

  auto onProblem = [](const mcap::Status& problem) {
    std::cerr << "Warning: MCAP parsing issue: " << problem.message << "\n";
  };

  // Paced on the monotonic capture clock: the log time is wall-clock time, and a clock
  // step during the recording (NTP settling on a freshly booted Jetson) would make the
  // replay skip ahead in a jump or stall. A recording without it falls back to log time.
  bool all_monotonic = true;
  auto capture_ns = [&](const trossen_sdk::Timestamp& ts, uint64_t log_time) -> uint64_t {
    if (ts.has_monotonic() && (ts.monotonic().seconds() > 0 || ts.monotonic().nanos() > 0)) {
      return static_cast<uint64_t>(ts.monotonic().seconds()) * 1000000000ULL +
             static_cast<uint64_t>(ts.monotonic().nanos());
    }
    all_monotonic = false;
    return log_time;
  };

  for (const auto& messageView : reader.readMessages(onProblem)) {
    const mcap::ChannelId ch_id = messageView.channel->id;

    // Arm joint state
    auto arm_it = channel_id_to_stream.find(ch_id);
    if (arm_it != channel_id_to_stream.end()) {
      const std::string& stream_id = arm_it->second;
      trossen_sdk::msg::JointState js_msg;
      if (!js_msg.ParseFromArray(
            reinterpret_cast<const char*>(messageView.message.data),
            messageView.message.dataSize)) {
        std::cerr << "Warning: Failed to parse JointState for " << stream_id << "\n";
        continue;
      }
      JointStateMessage js;
      js.timestamp_ns = capture_ns(js_msg.ts(), messageView.message.logTime);
      js.log_time_ns = messageView.message.logTime;
      for (auto v : js_msg.positions()) js.positions.push_back(static_cast<double>(v));
      for (auto v : js_msg.velocities()) js.velocities.push_back(static_cast<double>(v));
      messages_by_stream[stream_id].push_back(js);
      ++total_messages;
      continue;
    }

    // Base odometry — store body-frame twist as velocities: [linear_x, linear_y, angular_z]
    auto odom_it = odom_channel_id_to_stream.find(ch_id);
    if (odom_it != odom_channel_id_to_stream.end()) {
      const std::string& stream_id = odom_it->second;
      trossen_sdk::msg::Odometry2D odom_msg;
      if (!odom_msg.ParseFromArray(
            reinterpret_cast<const char*>(messageView.message.data),
            messageView.message.dataSize)) {
        std::cerr << "Warning: Failed to parse Odometry2D for " << stream_id << "\n";
        continue;
      }
      JointStateMessage js;
      js.timestamp_ns = capture_ns(odom_msg.ts(), messageView.message.logTime);
      js.log_time_ns = messageView.message.logTime;
      js.velocities = {
        static_cast<double>(odom_msg.twist().linear_x()),
        static_cast<double>(odom_msg.twist().linear_y()),
        static_cast<double>(odom_msg.twist().angular_z())
      };
      js.lift_velocity = static_cast<double>(odom_msg.lift_velocity());
      messages_by_stream[stream_id].push_back(js);
      ++total_messages;
    }
  }

  // One clock for every stream, or the streams would not line up with each other.
  if (!all_monotonic) {
    std::cerr << "Warning: some messages have no monotonic timestamp; pacing on the "
              << "MCAP log time, which a clock step during the recording disturbs\n";
    for (auto& [stream_id, messages] : messages_by_stream) {
      (void)stream_id;
      for (auto& m : messages) m.timestamp_ns = m.log_time_ns;
    }
  }

  std::cout << "  [ok] Parsed " << total_messages << " messages\n";
  for (const auto& [stream_id, messages] : messages_by_stream) {
    std::cout << "    - " << stream_id << ": " << messages.size() << " messages";
    if (base_stream_ids.count(stream_id)) std::cout << "  [base]";
    std::cout << "\n";
  }

  // Initialize the bases for the recorded base streams. A stream is driven by
  // whichever of `slates` or `trossen_bases` lists its stream_id.
  if (!base_stream_ids.empty()) {
    std::cout << "\nInitializing bases...\n";
    for (const auto& stream_id : base_stream_ids) {
      bool found_config = false;
      for (const auto& slate_cfg : cfg.slates) {
        if (slate_cfg.stream_id == stream_id) {
          found_config = true;
          try {
            auto slate_component =
              std::make_shared<trossen::hw::base::SlateBaseComponent>(stream_id);
            nlohmann::json hw_cfg = {
              {"reset_odometry", slate_cfg.reset_odometry},
              {"enable_torque", slate_cfg.enable_torque}
            };
            slate_component->configure(hw_cfg);
            auto slate_driver = slate_component->get_driver();
            if (slate_driver) {
              slate_drivers[stream_id] = slate_driver;
              std::cout << "  [ok] " << stream_id << " initialized\n";
            } else {
              std::cerr << "  [FAILED] Failed to get driver for " << stream_id << "\n";
            }
          } catch (const std::exception& e) {
            std::cerr << "  [FAILED] Failed to initialize " << stream_id
                      << ": " << e.what() << "\n";
          }
          break;
        }
      }
      for (const auto& base_cfg : cfg.trossen_bases) {
        if (base_cfg.stream_id != stream_id) continue;
        found_config = true;
#ifdef TROSSEN_ENABLE_RIVET
        try {
          // configure() homes the swerve modules unless the config sets
          // home_on_configure to false, which takes tens of seconds.
          std::cout << "  Configuring " << stream_id << " (Rivet base)...\n";
          auto base_component =
            std::make_shared<trossen::hw::base::TrossenBaseComponent>(stream_id);
          base_component->configure(base_cfg.hardware);
          trossen_bases[stream_id] = base_component;
          std::cout << "  [ok] " << stream_id << " initialized\n";
        } catch (const std::exception& e) {
          std::cerr << "  [FAILED] Failed to initialize " << stream_id
                    << ": " << e.what() << "\n";
        }
#else
        std::cerr << "  [skip] " << stream_id << " is a Rivet base, which needs a build with "
                  << "-DTROSSEN_ENABLE_RIVET=ON\n";
#endif
        break;
      }
      if (!found_config) {
        std::cout << "  [info] Skipping " << stream_id << " (no configuration provided)\n";
      }
    }
  }

#ifdef TROSSEN_ENABLE_RIVET
  // Base velocities are scaled with the timeline (below), so a speed that pushes the
  // recorded peak past a base's limit would be clamped and the base would fall short of
  // the recorded path while the arms replay theirs. Refused rather than clamped. The
  // recording is measured velocity, which can sit a little above the command limit it
  // was driven at, so 5% over is still accepted and clamped.
  constexpr double kLimitTolerance = 1.05;
  for (const auto& [stream_id, base] : trossen_bases) {
    const double k = cfg.playback_speed / kLimitTolerance;
    const auto info = base->get_info();
    const double max_linear = info.value("max_linear_mps", 0.0);
    const double max_angular = info.value("max_angular_rps", 0.0);
    const double max_lift = info.value("max_lift_mps", 0.0);
    double peak_linear = 0.0, peak_angular = 0.0, peak_lift = 0.0;
    for (const auto& m : messages_by_stream[stream_id]) {
      if (m.velocities.size() < 3) continue;
      peak_linear =
        std::max({peak_linear, std::abs(m.velocities[0]), std::abs(m.velocities[1])});
      peak_angular = std::max(peak_angular, std::abs(m.velocities[2]));
      peak_lift = std::max(peak_lift, std::abs(m.lift_velocity));
    }
    if (peak_linear * k > max_linear || peak_angular * k > max_angular ||
        peak_lift * k > max_lift) {
      const double v = cfg.playback_speed;
      std::cerr << "Error: at " << v << "x the recorded " << stream_id
                << " velocities exceed its limits (peak linear " << peak_linear * v << " of "
                << max_linear << " m/s, angular " << peak_angular * v << " of " << max_angular
                << " rad/s, lift " << peak_lift * v << " of " << max_lift
                << " m/s). Lower --speed.\n";
      return 1;
    }
  }
#endif

  // Calculate actual frequency from timestamps
  float recorded_fps = 0.0f;
  if (!messages_by_stream.empty()) {
    const auto& first_stream_messages = messages_by_stream.begin()->second;
    if (first_stream_messages.size() >= 2) {
      uint64_t total_duration_ns = first_stream_messages.back().timestamp_ns -
                                    first_stream_messages.front().timestamp_ns;
      double duration_s = total_duration_ns / 1e9;
      recorded_fps = (first_stream_messages.size() - 1) / duration_s;
      std::cout << "\nDetected recording frequency: " << std::fixed << std::setprecision(1)
                << recorded_fps << " Hz\n";
    }
  }

  // ──────────────────────────────────────────────────────────
  // Replay joint states
  // ──────────────────────────────────────────────────────────

  // Warn if no configured stream IDs appear in the MCAP
  {
    bool any_match = false;
    for (const auto& arm_cfg : cfg.arms) {
      if (messages_by_stream.count(arm_cfg.stream_id)) {
        any_match = true;
        break;
      }
    }
    if (!any_match && !cfg.arms.empty()) {
      std::cerr << "\nWarning: none of the configured arm stream IDs appear in this MCAP.\n";
      std::cerr << "  Configured: ";
      for (const auto& a : cfg.arms) std::cerr << "'" << a.stream_id << "' ";
      std::cerr << "\n  MCAP streams: ";
      for (const auto& [sid, _] : messages_by_stream) std::cerr << "'" << sid << "' ";
      std::cerr << "\nCheck that your config stream_ids match those recorded in the episode.\n\n";
    }
  }

  std::cout << "\nMoving arms to first recorded positions...\n";
  bool any_arm_moved = false;
  for (const auto& [stream_id, messages] : messages_by_stream) {
    if (!messages.empty() && drivers.find(stream_id) != drivers.end()) {
      drivers[stream_id]->set_all_positions(messages[0].positions, moving_time_s, false);
      any_arm_moved = true;
    }
  }
  if (any_arm_moved) {
    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
    std::cout << "  [ok] Arms moved to starting positions\n";
  } else {
    std::cout << "  [skip] No matching arm streams found — skipping pre-positioning\n";
  }

  if (!slate_drivers.empty()) {
    std::cout << "  [ok] SLATE bases ready (starting from zero velocity)\n";
  }
#ifdef TROSSEN_ENABLE_RIVET
  if (!trossen_bases.empty()) {
    std::cout << "  [ok] Rivet bases ready (starting from zero velocity)\n";
  }
#endif

  std::cout << "\nStarting replay in 3 seconds...\n";
  std::cout << "Press Ctrl+C to stop\n\n";
  std::this_thread::sleep_for(std::chrono::seconds(3));

  // Every stream is replayed on its own recorded timestamps, measured from the
  // earliest sample of any driven stream and scaled by playback_speed. Streams
  // recorded at different rates therefore stay in step with each other.
  auto has_driver = [&](const std::string& stream_id) {
#ifdef TROSSEN_ENABLE_RIVET
    if (trossen_bases.count(stream_id)) return true;
#endif
    return drivers.count(stream_id) > 0 || slate_drivers.count(stream_id) > 0;
  };

  uint64_t t_first = std::numeric_limits<uint64_t>::max();
  uint64_t t_last = 0;
  for (const auto& [stream_id, messages] : messages_by_stream) {
    if (messages.empty() || !has_driver(stream_id)) continue;
    t_first = std::min(t_first, messages.front().timestamp_ns);
    t_last = std::max(t_last, messages.back().timestamp_ns);
  }

  if (t_first == std::numeric_limits<uint64_t>::max()) {
    std::cerr << "Error: No messages to replay\n";
    return 1;
  }

  const double speed = cfg.playback_speed;
  const double span_s = static_cast<double>(t_last - t_first) / 1e9;
  std::cout << "Replaying " << std::fixed << std::setprecision(1) << span_s << " s at "
            << speed << "x on recorded timestamps\n";

  // At any speed but 1x each recorded sample is held for 1/speed of its recorded
  // interval, so the base velocities are scaled by the same factor to cover the
  // recorded distance, and the arm goal time is scaled so each move still takes about
  // one sample interval.
  auto send = [&](const std::string& stream_id, const JointStateMessage& msg) {
    if (drivers.count(stream_id)) {
      drivers[stream_id]->set_all_positions(
        msg.positions, driver_goal_times[stream_id] / speed, false);
      return;
    }
#ifdef TROSSEN_ENABLE_RIVET
    if (trossen_bases.count(stream_id)) {
      // Recorded twist is [linear_x, linear_y, angular_z]; write() takes
      // base_axis order [linear, angular, lift, lateral].
      if (msg.velocities.size() >= 3) {
        trossen_bases[stream_id]->write({
          static_cast<float>(msg.velocities[0] * speed),
          static_cast<float>(msg.velocities[2] * speed),
          static_cast<float>(msg.lift_velocity * speed),
          static_cast<float>(msg.velocities[1] * speed)});
      }
      return;
    }
#endif
    if (msg.velocities.size() >= 2) {
      base_driver::ChassisData cmd_data = {};
      cmd_data.cmd_vel_x = static_cast<float>(msg.velocities[0] * speed);

      if (msg.velocities.size() == 2) {
        cmd_data.cmd_vel_y = 0.0f;
        cmd_data.cmd_vel_z = static_cast<float>(msg.velocities[1] * speed);
      } else {
        cmd_data.cmd_vel_y = static_cast<float>(msg.velocities[1] * speed);
        cmd_data.cmd_vel_z = static_cast<float>(msg.velocities[2] * speed);
      }

      cmd_data.light_state = static_cast<uint32_t>(LightState::WHITE);
      slate_drivers[stream_id]->write(cmd_data);
    }
  };

  std::map<std::string, size_t> stream_indices;
  for (const auto& [stream_id, messages] : messages_by_stream) {
    if (has_driver(stream_id)) stream_indices[stream_id] = 0;
  }

  // Wall-clock instant at which a recorded timestamp is due.
  const auto replay_start = std::chrono::steady_clock::now();
  auto due_at = [&](uint64_t timestamp_ns) {
    const double offset_s = static_cast<double>(timestamp_ns - t_first) / 1e9 / speed;
    return replay_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(offset_s));
  };

  size_t messages_replayed = 0;
  int last_progress = -1;
  bool replay_failed = false;

  // An arm fault or a CAN error throws out of send(). Caught so the teardown below
  // still stops the base and returns the arms; an exception escaping main() would
  // terminate without unwinding, leaving only the base firmware's heartbeat timeout
  // to stop wheels that hold their last velocity.
  try {
    while (!trossen::utils::g_stop_requested) {
      const auto now = std::chrono::steady_clock::now();
      bool all_streams_done = true;
      auto next_due = std::chrono::steady_clock::time_point::max();

      for (auto& [stream_id, idx] : stream_indices) {
        const auto& messages = messages_by_stream[stream_id];
        if (idx >= messages.size()) continue;
        all_streams_done = false;

        // Send the newest sample that is due. If the loop fell behind, the older
        // ones are skipped rather than sent in a burst.
        size_t newest_due = messages.size();
        while (idx < messages.size() && due_at(messages[idx].timestamp_ns) <= now) {
          newest_due = idx++;
        }
        if (newest_due < messages.size()) {
          send(stream_id, messages[newest_due]);
          ++messages_replayed;
        }
        if (idx < messages.size()) {
          next_due = std::min(next_due, due_at(messages[idx].timestamp_ns));
        }
      }

      if (all_streams_done) {
        break;
      }

      const double elapsed_s = std::chrono::duration<double>(now - replay_start).count() * speed;
      const int progress = span_s > 0.0 ? static_cast<int>(100.0 * elapsed_s / span_s) : 100;
      if (progress != last_progress) {
        last_progress = progress;
        std::cout << "\rProgress: " << std::min(progress, 100) << "% (" << messages_replayed
                  << " messages)    " << std::flush;
      }

      // Woken at least every 50 ms so Ctrl+C is answered during a long recording gap.
      if (next_due != std::chrono::steady_clock::time_point::max()) {
        std::this_thread::sleep_until(
          std::min(next_due, std::chrono::steady_clock::now() + std::chrono::milliseconds(50)));
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "\n\nError during replay: " << e.what() << "\n";
    replay_failed = true;
  }

  std::cout << "\n\n" << (replay_failed ? "Replay stopped." : "Replay complete!") << "\n";
  std::cout << "Total messages replayed: " << messages_replayed << "\n";

  // Each step is attempted even if an earlier one throws, so a faulted arm cannot keep
  // a base from being stopped. The bases go first.
  auto attempt = [](const std::string& what, const std::function<void()>& step) {
    try {
      step();
    } catch (const std::exception& e) {
      std::cerr << "Error: " << what << ": " << e.what() << "\n";
    }
  };

#ifdef TROSSEN_ENABLE_RIVET
  if (!trossen_bases.empty()) {
    std::cout << "Stopping Rivet bases...\n";
    for (auto& [stream_id, base] : trossen_bases) {
      attempt("stopping " + stream_id, [&] { base->end_teleop(); });
    }
  }
#endif

  if (!slate_drivers.empty()) {
    std::cout << "Stopping SLATE bases...\n";
    for (auto& [stream_id, driver] : slate_drivers) {
      attempt("stopping " + stream_id, [&] {
        base_driver::ChassisData stop_cmd = {};
        stop_cmd.cmd_vel_x = 0.0f;
        stop_cmd.cmd_vel_y = 0.0f;
        stop_cmd.cmd_vel_z = 0.0f;
        stop_cmd.light_state = static_cast<uint32_t>(LightState::GREEN);
        driver->write(stop_cmd);
      });
    }
  }

  std::cout << "\nReturning arms to sleep positions...\n";
  for (auto& [stream_id, driver] : drivers) {
    attempt("returning " + stream_id + " to sleep", [&] {
      driver->set_all_positions(
        std::vector<double>(driver->get_num_joints(), 0.0),
        2.0f,
        false);
    });
  }
  std::this_thread::sleep_for(std::chrono::seconds(2));

  std::cout << "Done!\n";
  return replay_failed ? 1 : 0;
}
