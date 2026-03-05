/**
 * @file replay_trossen_mcap_jointstate.cpp
 * @brief Replay joint states from a TrossenMCAP file to robot arms
 *
 * Reads joint state data from an MCAP file and replays it on the configured
 * robot arms. Stream IDs in the recording are matched to the configured arms.
 *
 * Usage:
 *   ./replay_trossen_mcap_jointstate <path_to_mcap_file> [options]
 *
 * Options:
 *   --config <path>   Config JSON file (default: scripts/replay_config.json)
 *   --set KEY=VALUE   Override a config value (repeatable)
 *   --speed <float>   Playback speed multiplier (default: from config)
 *   --help            Show this help message
 *
 * Examples:
 *   ./replay_trossen_mcap_jointstate ~/datasets/episode_000000.mcap
 *   ./replay_trossen_mcap_jointstate ~/datasets/episode.mcap --speed 0.5
 *   ./replay_trossen_mcap_jointstate ~/datasets/episode.mcap --config my_config.json
 */

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
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
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/configuration/cli_parser.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"

#include "JointState.pb.h"
#include "demo_utils.hpp"

struct ArmConfig {
  std::string stream_id;
  std::string ip_address;
  std::string model;
  std::string end_effector;
};

struct SlateConfig {
  std::string stream_id;
  bool reset_odometry = false;
  bool enable_torque = true;
};

struct ReplayConfig {
  std::string mcap_file;
  std::vector<ArmConfig> arms;
  std::vector<SlateConfig> slates;
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
    std::cerr << "  " << argv[0] << " ~/datasets/episode_000000.mcap\n";
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

  trossen::demo::print_config_banner("MCAP Joint State Replay", config_lines);
  trossen::demo::install_signal_handler();

  // ──────────────────────────────────────────────────────────
  // Initialize hardware
  // ──────────────────────────────────────────────────────────

  std::map<std::string, std::shared_ptr<trossen_arm::TrossenArmDriver>> drivers;
  std::map<std::string, std::shared_ptr<trossen::hw::HardwareComponent>> components;
  std::map<std::string, std::shared_ptr<trossen_slate::TrossenSlate>> slate_drivers;

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

  // Stage arms to starting position
  const float moving_time_s = 2.0f;
  std::cout << "\nStaging arms to starting positions...\n";
  for (auto& [stream_id, driver] : drivers) {
    driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);
  }
  std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
  std::cout << "  [ok] Arms staged to ready position\n";

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
    uint64_t timestamp_ns;
    std::vector<double> positions;
    std::vector<double> velocities;
  };

  std::map<std::string, std::vector<JointStateMessage>> messages_by_stream;
  std::map<mcap::ChannelId, std::string> channel_id_to_stream;

  std::cout << "  Available channels in MCAP file:\n";
  for (const auto& [channel_id, channel_ptr] : reader.channels()) {
    std::cout << "    - Topic: '" << channel_ptr->topic << "' (Schema: "
              << channel_ptr->schemaId << ", ID: " << channel_id << ")\n";
  }

  std::set<std::string> slate_stream_ids;
  for (const auto& [channel_id, channel_ptr] : reader.channels()) {
    std::string topic = channel_ptr->topic;
    size_t pos = topic.find("/joints/state");
    if (pos != std::string::npos) {
      std::string stream_id = topic.substr(0, pos);
      channel_id_to_stream[channel_id] = stream_id;
      std::cout << "  Found joint state channel: " << topic
                << " (stream_id: " << stream_id << ")\n";
    }
  }

  if (channel_id_to_stream.empty()) {
    std::cerr << "Error: No joint state channels found in MCAP file\n";
    std::cerr << "Looking for topics matching pattern: '{stream_id}/joints/state'\n";
    return 1;
  }

  std::cout << "\nParsing joint state messages...\n";
  size_t total_messages = 0;

  auto onProblem = [](const mcap::Status& problem) {
    std::cerr << "Warning: MCAP parsing issue: " << problem.message << "\n";
  };

  for (const auto& messageView : reader.readMessages(onProblem)) {
    auto it = channel_id_to_stream.find(messageView.channel->id);
    if (it == channel_id_to_stream.end()) {
      continue;
    }

    const std::string& stream_id = it->second;

    trossen_sdk::msg::JointState js_msg;
    if (!js_msg.ParseFromArray(
          reinterpret_cast<const char*>(messageView.message.data),
          messageView.message.dataSize)) {
      std::cerr << "Warning: Failed to parse message for " << stream_id << "\n";
      continue;
    }

    JointStateMessage js;
    js.timestamp_ns = messageView.message.logTime;
    for (auto v : js_msg.positions()) {
      js.positions.push_back(static_cast<double>(v));
    }
    for (auto v : js_msg.velocities()) {
      js.velocities.push_back(static_cast<double>(v));
    }

    if (js.positions.empty() && !js.velocities.empty() &&
        (js.velocities.size() == 2 || js.velocities.size() == 3)) {
      slate_stream_ids.insert(stream_id);
    }

    messages_by_stream[stream_id].push_back(js);
    ++total_messages;
  }

  std::cout << "  [ok] Parsed " << total_messages << " joint state messages\n";
  for (const auto& [stream_id, messages] : messages_by_stream) {
    std::cout << "    - " << stream_id << ": " << messages.size() << " messages\n";
  }

  // Initialize SLATE bases if detected
  if (!slate_stream_ids.empty()) {
    std::cout << "\nInitializing SLATE bases...\n";
    for (const auto& stream_id : slate_stream_ids) {
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
      if (!found_config) {
        std::cout << "  [info] Skipping " << stream_id << " (no configuration provided)\n";
      }
    }
  }

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

  std::cout << "\nMoving arms to first recorded positions...\n";
  for (const auto& [stream_id, messages] : messages_by_stream) {
    if (!messages.empty() && drivers.find(stream_id) != drivers.end()) {
      drivers[stream_id]->set_all_positions(messages[0].positions, moving_time_s, false);
    }
  }
  std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
  std::cout << "  [ok] Arms moved to starting positions\n";

  if (!slate_drivers.empty()) {
    std::cout << "  [ok] SLATE bases ready (starting from zero velocity)\n";
  }

  std::cout << "\nStarting replay in 3 seconds...\n";
  std::cout << "Press Ctrl+C to stop\n\n";
  std::this_thread::sleep_for(std::chrono::seconds(3));

  std::string reference_stream;
  size_t max_messages = 0;
  for (const auto& [stream_id, messages] : messages_by_stream) {
    if (messages.size() > max_messages) {
      max_messages = messages.size();
      reference_stream = stream_id;
    }
  }

  if (reference_stream.empty() || max_messages == 0) {
    std::cerr << "Error: No messages to replay\n";
    return 1;
  }

  double replay_fps = recorded_fps * cfg.playback_speed;
  if (replay_fps <= 0.0f) {
    replay_fps = 30.0;
  }
  auto frame_duration = std::chrono::duration<double>(1.0 / replay_fps);

  std::cout << "Replaying at " << std::fixed << std::setprecision(1) << replay_fps
            << " Hz (reference: " << reference_stream << ", " << max_messages << " frames)\n";

  std::map<std::string, size_t> stream_indices;
  for (const auto& [stream_id, _] : messages_by_stream) {
    stream_indices[stream_id] = 0;
  }

  size_t messages_replayed = 0;
  auto next_frame_time = std::chrono::steady_clock::now();

  while (!trossen::demo::g_stop_requested) {
    bool all_streams_done = true;
    for (auto& [stream_id, idx] : stream_indices) {
      const auto& messages = messages_by_stream[stream_id];

      if (idx < messages.size()) {
        const auto& msg = messages[idx];

        if (drivers.find(stream_id) != drivers.end()) {
          drivers[stream_id]->set_all_positions(msg.positions, 0.0f, false);
          ++idx;
          ++messages_replayed;
          all_streams_done = false;
        } else if (slate_drivers.find(stream_id) != slate_drivers.end()) {
          if (msg.velocities.size() >= 2) {
            base_driver::ChassisData cmd_data = {};
            cmd_data.cmd_vel_x = static_cast<float>(msg.velocities[0]);

            if (msg.velocities.size() == 2) {
              cmd_data.cmd_vel_y = 0.0f;
              cmd_data.cmd_vel_z = static_cast<float>(msg.velocities[1]);
            } else {
              cmd_data.cmd_vel_y = static_cast<float>(msg.velocities[1]);
              cmd_data.cmd_vel_z = static_cast<float>(msg.velocities[2]);
            }

            cmd_data.light_state = static_cast<uint32_t>(LightState::WHITE);
            slate_drivers[stream_id]->write(cmd_data);
          }
          ++idx;
          ++messages_replayed;
          all_streams_done = false;
        } else {
          ++idx;
        }
      }
    }

    if (all_streams_done) {
      break;
    }

    if (stream_indices[reference_stream] % 10 == 0) {
      float progress = 100.0f * stream_indices[reference_stream] / max_messages;
      std::cout << "\rProgress: " << std::fixed << std::setprecision(1)
                << progress << "% (" << messages_replayed << " messages)    " << std::flush;
    }

    next_frame_time += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      frame_duration);
    std::this_thread::sleep_until(next_frame_time);
  }

  std::cout << "\n\nReplay complete!\n";
  std::cout << "Total messages replayed: " << messages_replayed << "\n";

  std::cout << "\nReturning arms to sleep positions...\n";
  for (auto& [stream_id, driver] : drivers) {
    driver->set_all_positions(
      std::vector<double>(driver->get_num_joints(), 0.0),
      2.0f,
      false);
  }
  std::this_thread::sleep_for(std::chrono::seconds(2));

  if (!slate_drivers.empty()) {
    std::cout << "Stopping SLATE bases...\n";
    for (auto& [stream_id, driver] : slate_drivers) {
      base_driver::ChassisData stop_cmd = {};
      stop_cmd.cmd_vel_x = 0.0f;
      stop_cmd.cmd_vel_y = 0.0f;
      stop_cmd.cmd_vel_z = 0.0f;
      stop_cmd.light_state = static_cast<uint32_t>(LightState::GREEN);
      driver->write(stop_cmd);
    }
  }

  std::cout << "Done!\n";
  return 0;
}
