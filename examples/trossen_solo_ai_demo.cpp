/**
 * @file trossen_solo_ai_demo.cpp
 * @brief Trossen AI Solo configuration reader and episode recorder
 *
 * This demo reads the Trossen AI Solo robot configuration and will be used
 * to record episodes for the solo robot setup (1 leader + 1 follower arm).
 *
 * Usage:
 *   ./trossen_solo_ai_demo
 *   ./trossen_solo_ai_demo --config config/robot_configs.json
 */

#include <iostream>
#include <string>
#include <exception>

#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/configuration/types/robots/trossen_solo_ai_config.hpp"

void print_banner() {
  std::cout << "\n";
  std::cout << "═══════════════════════════════════════════════════════════\n";
  std::cout << "  Trossen AI Solo - Configuration Reader\n";
  std::cout << "═══════════════════════════════════════════════════════════\n";
  std::cout << "\n";
}

void print_config(const trossen::configuration::TrossenAiSoloConfig& config) {
  std::cout << "Robot Configuration:\n";
  std::cout << "  Name: " << config.robot_name << "\n";
  std::cout << "  Type: " << config.type() << "\n\n";

  std::cout << "Arms (" << config.arms.size() << "):\n";
  for (size_t i = 0; i < config.arms.size(); ++i) {
    const auto& arm = config.arms[i];
    std::cout << "  [" << i << "] Model: " << arm.model << "\n";
    std::cout << "      IP Address: " << arm.ip_address << "\n";
    std::cout << "      End Effector: " << arm.end_effector << "\n";
    std::cout << "      Joint Rate: " << arm.joint_rate_hz << " Hz\n";
    std::cout << "      Clear Error: " << (arm.clear_error ? "Yes" : "No") << "\n";
    if (!arm.id.empty()) {
      std::cout << "      ID: " << arm.id << "\n";
    }
    std::cout << "\n";
  }

  std::cout << "Cameras (" << config.cameras.size() << "):\n";
  for (size_t i = 0; i < config.cameras.size(); ++i) {
    const auto& cam = config.cameras[i];
    std::cout << "  [" << i << "] Type: " << cam.type << "\n";
    std::cout << "      Device ID: " << cam.unique_device_id << "\n";
    std::cout << "      Resolution: " << cam.width << "x" << cam.height << "\n";
    std::cout << "      FPS: " << cam.fps << "\n";
    std::cout << "      Encoding: " << cam.encoding << "\n";
    std::cout << "      Use Device Time: " << (cam.use_device_time ? "Yes" : "No") << "\n";
    if (cam.enable_depth) {
      std::cout << "      Depth Enabled: Yes\n";
    }
    if (!cam.id.empty()) {
      std::cout << "      ID: " << cam.id << "\n";
    }
    std::cout << "\n";
  }
}

int main(int argc, char** argv) {
  print_banner();

  // Parse command line arguments
  std::string config_file = "config/robot_configs.json";
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--config" && i + 1 < argc) {
      config_file = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: " << argv[0] << " [options]\n\n";
      std::cout << "Options:\n";
      std::cout << "  --config <file>  Configuration file path "
                << "(default: config/robot_configs.json)\n";
      std::cout << "  --help, -h       Show this help message\n";
      return 0;
    }
  }

  try {
    // Load configuration file
    std::cout << "Loading configuration from: " << config_file << "\n\n";
    auto config_json = trossen::configuration::JsonLoader::load(config_file);

    // Parse Trossen AI Solo configuration
    auto robot_config = trossen::configuration::TrossenAiSoloConfig::from_json(
      config_json["trossen_solo_ai"]);

    // Display configuration
    print_config(robot_config);

    std::cout << "Configuration loaded successfully!\n";
    std::cout << "\nNote: Episode recording functionality will be "
              << "implemented in future versions.\n";
  } catch (const std::exception& e) {
    std::cerr << "\nError: " << e.what() << "\n";
    std::cerr << "Failed to load configuration.\n";
    return 1;
  }

  return 0;
}
