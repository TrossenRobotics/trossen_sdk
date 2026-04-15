/**
 * @file trossen_keyboard_teleop.cpp
 * @brief Drive a Trossen follower arm in cartesian space from the keyboard.
 *
 * Minimal teleop demo: a `keyboard_teleop` virtual leader emits cartesian
 * pose updates as the user presses WASDRF; a `trossen_arm` follower
 * mirrors them via TeleopController. No recording, no cameras — just the
 * mirror loop.
 *
 * Usage:
 *   ./trossen_keyboard_teleop [--config PATH] [--set KEY=VALUE]
 *
 * Key bindings (consumed by KeyboardTeleopComponent):
 *   W / S    +X / -X   (forward / backward)
 *   A / D    -Y / +Y   (left / right)
 *   R / F    +Z / -Z   (raise / fall)
 *   Ctrl+C   stop
 */

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "trossen_sdk/configuration/cli_parser.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/configuration/sdk_config.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/teleop/teleop_factory.hpp"
#include "trossen_sdk/utils/app_utils.hpp"

namespace {

void print_banner() {
  std::cout << "\n";
  std::cout << "──────────────────────────────────────────────────────────\n";
  std::cout << "  Keyboard teleop demo (cartesian)\n";
  std::cout << "──────────────────────────────────────────────────────────\n";
  std::cout << "  W / S    forward / backward (X)\n";
  std::cout << "  A / D    left    / right    (Y)\n";
  std::cout << "  R / F    raise   / fall     (Z)\n";
  std::cout << "  Ctrl+C   stop\n";
  std::cout << "──────────────────────────────────────────────────────────\n\n";
}

}  // namespace

int main(int argc, char** argv) {
  trossen::configuration::CliParser cli(argc, argv);

  const std::string config_path = cli.get_string(
    "config", "examples/trossen_keyboard_teleop/config.json");

  if (!std::filesystem::exists(config_path)) {
    std::cerr << "Error: config file not found: " << config_path << "\n";
    return 1;
  }

  // Load JSON, apply --set overrides, populate GlobalConfig so the teleop
  // factory and HardwareRegistry can see everything.
  auto j = trossen::configuration::JsonLoader::load(config_path);
  const auto overrides = cli.get_set_overrides();
  if (!overrides.empty()) {
    j = trossen::configuration::merge_overrides(j, overrides);
  }
  auto cfg = trossen::configuration::SdkConfig::from_json(j);
  cfg.populate_global_config();

  trossen::utils::install_signal_handler();

  // ── Construct hardware via the registry ────────────────────────────────
  // Iterate raw JSON (not cfg.hardware.arms) so the per-arm `type` field
  // is honored — this demo mixes hardware kinds (keyboard_teleop +
  // trossen_arm) under `hardware.arms`.
  // mark_active=true so the teleop factory can resolve them by id.
  std::cout << "Initializing hardware...\n";
  if (j.contains("hardware") && j.at("hardware").contains("arms")) {
    for (auto it = j.at("hardware").at("arms").begin();
         it != j.at("hardware").at("arms").end(); ++it) {
      const std::string id = it.key();
      const auto& arm_j = it.value();
      const std::string type = arm_j.value("type", "trossen_arm");
      trossen::hw::HardwareRegistry::create(type, id, arm_j, /*mark_active=*/true);
      std::cout << "  [ok] [" << id << "] " << type << "\n";
    }
  }

  // ── Build controllers from config (one per teleop pair) ────────────────
  auto controllers = trossen::hw::teleop::create_controllers_from_global_config();
  if (controllers.empty()) {
    std::cerr << "Error: no teleop controllers built. Check `teleop.pairs`.\n";
    return 1;
  }

  // ── Run ────────────────────────────────────────────────────────────────
  // The keyboard component's poll thread starts inside prepare_for_teleop
  // and keeps emitting pose deltas until end_teleop runs at shutdown.
  for (auto& ctrl : controllers) {
    ctrl->prepare_teleop();
    ctrl->teleop();
  }

  print_banner();

  while (!trossen::utils::g_stop_requested) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::cout << "\nStopping...\n";
  for (auto& ctrl : controllers) {
    ctrl->stop_teleop();
  }
  return 0;
}
