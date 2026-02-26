/**
 * @file replay_mcap_jointstate.cpp
 * @brief Replay joint states from MCAP file to robot arms
 *
 * This example reads joint state data from an MCAP file and replays it on the corresponding
 * robot arms. It matches stream IDs from the recording to the configured arms.
 *
 * Usage:
 *   ./replay_mcap_jointstate <path_to_mcap_file> [playback_speed]
 *
 * Example:
 *   ./replay_mcap_jointstate ~/datasets/my_dataset/episode_000000.mcap 1.0
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
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"

#include "JointState.pb.h"
#include "./demo_utils.hpp"

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
  float playback_speed = 1.0f;  // 1.0 = real-time, 2.0 = 2x speed, 0.5 = half speed
};

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <path_to_mcap_file> [playback_speed]\n";
    std::cerr << "Example: " << argv[0] << " ~/datasets/my_dataset/episode_000000.mcap 1.0\n";
    return 1;
  }

  // Load global configuration
  if (!std::filesystem::exists("config/sdk_config.json")) {
    std::cerr << "Error: config/sdk_config.json not found!" << std::endl;
    return 1;
  }

  auto j = trossen::configuration::JsonLoader::load("config/sdk_config.json");
  trossen::configuration::GlobalConfig::instance().load_from_json(j);

  ReplayConfig cfg;
  cfg.mcap_file = argv[1];

  if (argc >= 3) {
    cfg.playback_speed = std::stof(argv[2]);
  }

  // Check if MCAP file exists
  if (!std::filesystem::exists(cfg.mcap_file)) {
    std::cerr << "Error: MCAP file not found: " << cfg.mcap_file << std::endl;
    return 1;
  }

  // Configure arms for replay (followers only)
  cfg.arms = {
    {"follower_left", "192.168.1.5", "wxai_v0", "wxai_v0_follower"},
    {"follower_right", "192.168.1.4", "wxai_v0", "wxai_v0_follower"}
  };

  // Configure SLATE bases for replay (if detected in MCAP)
  cfg.slates = {
    {"slate_base", false, true}  // Stream ID must match MCAP file
  };

  // Print configuration
  std::vector<std::string> config_lines = {
    "MCAP file:        " + cfg.mcap_file,
    "Playback speed:   " + std::to_string(cfg.playback_speed) + "x",
    "Arms configured:  " + std::to_string(cfg.arms.size())
  };

  for (const auto& arm : cfg.arms) {
    config_lines.push_back("  - " + arm.stream_id + " (" + arm.ip_address + ")");
  }

  trossen::demo::print_config_banner("MCAP Joint State Replay", config_lines);

  // Install signal handler for graceful shutdown
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
      std::cerr << "  ✗ Failed to create component for " << arm_cfg.stream_id << "\n";
      continue;
    }

    auto driver = arm_component->get_hardware();
    if (!driver) {
      std::cerr << "  ✗ Failed to get driver for " << arm_cfg.stream_id << "\n";
      continue;
    }

    drivers[arm_cfg.stream_id] = driver;
    components[arm_cfg.stream_id] = component;
    std::cout << "  ✓ " << arm_cfg.stream_id << " initialized (" << arm_cfg.ip_address << ")\n";
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

  // Stage arms to starting position (similar to recording setup)
  const float moving_time_s = 2.0f;
  std::cout << "\nStaging arms to starting positions...\n";
  for (auto& [stream_id, driver] : drivers) {
    driver->set_all_positions(trossen::demo::STAGED_POSITIONS, moving_time_s, false);
  }
  std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
  std::cout << "  ✓ Arms staged to ready position\n";

  // ──────────────────────────────────────────────────────────
  // Read MCAP file and parse joint states
  // ──────────────────────────────────────────────────────────

  std::cout << "\nReading MCAP file: " << cfg.mcap_file << "\n";

  // Open MCAP file
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

  // Read summary to populate channel information
  auto summary_status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
  if (!summary_status.ok()) {
    std::cerr << "Error: Failed to read MCAP summary: " << summary_status.message << "\n";
    return 1;
  }

  // Collect messages by channel
  struct JointStateMessage {
    uint64_t timestamp_ns;
    std::vector<double> positions;
    std::vector<double> velocities;
  };

  std::map<std::string, std::vector<JointStateMessage>> messages_by_stream;
  std::map<mcap::ChannelId, std::string> channel_id_to_stream;

  // First, list all available channels for debugging
  std::cout << "  Available channels in MCAP file:\n";
  for (const auto& [channel_id, channel_ptr] : reader.channels()) {
    std::cout << "    - Topic: '" << channel_ptr->topic << "' (Schema: "
              << channel_ptr->schemaId << ", ID: " << channel_id << ")\n";
  }

  // Map channel IDs to stream IDs and detect SLATE bases
  std::set<std::string> slate_stream_ids;
  for (const auto& [channel_id, channel_ptr] : reader.channels()) {
    std::string topic = channel_ptr->topic;
    // Extract stream_id from topic (format: "{stream_id}/joints/state")
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

  // Read all messages
  std::cout << "\nParsing joint state messages...\n";
  size_t total_messages = 0;

  auto onProblem = [](const mcap::Status& problem) {
    std::cerr << "Warning: MCAP parsing issue: " << problem.message << "\n";
  };

  for (const auto& messageView : reader.readMessages(onProblem)) {
    auto it = channel_id_to_stream.find(messageView.channel->id);
    if (it == channel_id_to_stream.end()) {
      continue;  // Not a joint state channel we care about
    }

    const std::string& stream_id = it->second;

    // Parse protobuf message (parse ALL joint state messages, not just arms)
    trossen_sdk::msg::JointState js_msg;
    if (!js_msg.ParseFromArray(
          reinterpret_cast<const char*>(messageView.message.data),
          messageView.message.dataSize)) {
      std::cerr << "Warning: Failed to parse message for " << stream_id << "\n";
      continue;
    }

    // Extract data (convert float to double)
    JointStateMessage js;
    js.timestamp_ns = messageView.message.logTime;
    for (auto v : js_msg.positions()) {
      js.positions.push_back(static_cast<double>(v));
    }
    for (auto v : js_msg.velocities()) {
      js.velocities.push_back(static_cast<double>(v));
    }

    // Detect SLATE bases (have velocities but no positions)
    // Velocities can be 2 values [linear_x, angular_z] or 3 values [linear_x, linear_y, angular_z]
    if (js.positions.empty() && !js.velocities.empty() &&
        (js.velocities.size() == 2 || js.velocities.size() == 3)) {
      slate_stream_ids.insert(stream_id);
    }

    messages_by_stream[stream_id].push_back(js);
    ++total_messages;
  }

  std::cout << "  ✓ Parsed " << total_messages << " joint state messages\n";
  for (const auto& [stream_id, messages] : messages_by_stream) {
    std::cout << "    - " << stream_id << ": " << messages.size() << " messages\n";
  }

  // Initialize SLATE bases if detected
  if (!slate_stream_ids.empty()) {
    std::cout << "\nInitializing SLATE bases...\n";
    for (const auto& stream_id : slate_stream_ids) {
      // Check if we have config for this SLATE
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
              std::cout << "  ✓ " << stream_id << " initialized\n";
            } else {
              std::cerr << "  ✗ Failed to get driver for " << stream_id << "\n";
            }
          } catch (const std::exception& e) {
            std::cerr << "  ✗ Failed to initialize " << stream_id << ": " << e.what() << "\n";
          }
          break;
        }
      }
      if (!found_config) {
        std::cout << "  ⓘ Skipping " << stream_id << " (no configuration provided)\n";
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

  // Move arms to first recorded position
  std::cout << "\nMoving arms to first recorded positions...\n";
  for (const auto& [stream_id, messages] : messages_by_stream) {
    if (!messages.empty() && drivers.find(stream_id) != drivers.end()) {
      drivers[stream_id]->set_all_positions(messages[0].positions, moving_time_s, false);
    }
  }
  std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
  std::cout << "  ✓ Arms moved to starting positions\n";

  // SLATE bases start at zero velocity
  if (!slate_drivers.empty()) {
    std::cout << "  ✓ SLATE bases ready (starting from zero velocity)\n";
  }

  std::cout << "\nStarting replay in 3 seconds...\n";
  std::cout << "Press Ctrl+C to stop\n\n";
  std::this_thread::sleep_for(std::chrono::seconds(3));

  // Find the stream with the most messages to use as reference
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

  // Calculate replay timing based on recorded frequency and playback speed
  double replay_fps = recorded_fps * cfg.playback_speed;
  if (replay_fps <= 0.0f) {
    replay_fps = 30.0;  // Default fallback
  }
  auto frame_duration = std::chrono::duration<double>(1.0 / replay_fps);

  std::cout << "Replaying at " << std::fixed << std::setprecision(1) << replay_fps
            << " Hz (reference: " << reference_stream << ", " << max_messages << " frames)\n";

  // Index trackers for each stream
  std::map<std::string, size_t> stream_indices;
  for (const auto& [stream_id, _] : messages_by_stream) {
    stream_indices[stream_id] = 0;
  }

  size_t messages_replayed = 0;
  auto next_frame_time = std::chrono::steady_clock::now();

  while (!trossen::demo::g_stop_requested) {
    // Send next message for each stream
    bool all_streams_done = true;
    for (auto& [stream_id, idx] : stream_indices) {
      const auto& messages = messages_by_stream[stream_id];

      if (idx < messages.size()) {
        const auto& msg = messages[idx];

        // Check if this is an arm or a SLATE base (skip if neither)
        if (drivers.find(stream_id) != drivers.end()) {
          // Arm: send position commands
          drivers[stream_id]->set_all_positions(msg.positions, 0.0f, false);
          ++idx;
          ++messages_replayed;
          all_streams_done = false;
        } else if (slate_drivers.find(stream_id) != slate_drivers.end()) {
          // SLATE base: send velocity commands
          // Format: velocities[0] = linear_x, velocities[1] = angular_z (2 values)
          // Or: velocities[0] = linear_x, velocities[1] = linear_y,
          //     velocities[2] = angular_z (3 values)
          if (msg.velocities.size() >= 2) {
            base_driver::ChassisData cmd_data = {};
            cmd_data.cmd_vel_x = static_cast<float>(msg.velocities[0]);

            if (msg.velocities.size() == 2) {
              // 2-value format: [linear_x, angular_z]
              cmd_data.cmd_vel_y = 0.0f;
              cmd_data.cmd_vel_z = static_cast<float>(msg.velocities[1]);
            } else {
              // 3-value format: [linear_x, linear_y, angular_z]
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
          // Stream has no driver, skip it
          ++idx;
        }
      }
    }

    // Check if all streams are done
    if (all_streams_done) {
      break;
    }

    // Update progress every 10 frames
    if (stream_indices[reference_stream] % 10 == 0) {
      float progress = 100.0f * stream_indices[reference_stream] / max_messages;
      std::cout << "\rProgress: " << std::fixed << std::setprecision(1)
                << progress << "% (" << messages_replayed << " messages)    " << std::flush;
    }

    // Wait until next frame time
    next_frame_time += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      frame_duration);
    std::this_thread::sleep_until(next_frame_time);
  }

  std::cout << "\n\nReplay complete!\n";
  std::cout << "Total messages replayed: " << messages_replayed << "\n";

  // Return arms to sleep position
  std::cout << "\nReturning arms to sleep positions...\n";
  for (auto& [stream_id, driver] : drivers) {
    driver->set_all_positions(
      std::vector<double>(driver->get_num_joints(), 0.0),
      2.0f,
      false);
  }
  std::this_thread::sleep_for(std::chrono::seconds(2));

  // Stop SLATE bases
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
