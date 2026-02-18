/**
 * @file teleop_arm_component_demo.cpp
 * @brief Minimal demo showcasing TeleopArmComponent with leader-follower configuration
 */

#include <iostream>

#include "nlohmann/json.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/arm/teleop_arm_component.hpp"

int main(int argc, char** argv) {
  // Parse command line arguments
  std::string leader_ip = "192.168.1.2";
  std::string follower_ip = "192.168.1.4";

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--leader-ip" && i + 1 < argc) {
      leader_ip = argv[++i];
    } else if (arg == "--follower-ip" && i + 1 < argc) {
      follower_ip = argv[++i];
    } else if (arg == "--help") {
      std::cout << "Usage: " << argv[0] << " [OPTIONS]\n";
      std::cout << "Options:\n";
      std::cout << "  --leader-ip <IP>     Leader arm IP address (default: 192.168.1.2)\n";
      std::cout << "  --follower-ip <IP>   Follower arm IP address (default: 192.168.1.4)\n";
      std::cout << "  --help               Show this help message\n";
      return 0;
    }
  }

  std::cout << "\nTeleop Arm Component Demo\n";
  std::cout << "==========================\n\n";

  // Create TeleopArmComponent from configuration
  std::cout << "1. Creating TeleopArmComponent...\n";

  nlohmann::json teleop_config = {
    {"leader", {
      {"ip_address", leader_ip},
      {"model", "wxai_v0"},
      {"end_effector", "wxai_v0_leader"}
    }},
    {"follower", {
      {"ip_address", follower_ip},
      {"model", "wxai_v0"},
      {"end_effector", "wxai_v0_follower"}
    }}
  };

  try {
    // Create and auto-register the component
    auto teleop_component = trossen::hw::HardwareRegistry::create(
      "teleop_arm", "teleop_arm_pair", teleop_config);
    std::cout << "   ✓ Created and registered\n";

    // Retrieve from active registry and get drivers
    std::cout << "\n2. Accessing drivers...\n";
    auto retrieved = trossen::hw::ActiveHardwareRegistry::get_as<
      trossen::hw::arm::TeleopArmComponent>("teleop_arm_pair");

    auto drivers = retrieved->get_hardware();
    std::cout << "   ✓ Leader driver: "
              << (drivers.leader ? "available" : "unavailable") << "\n";
    std::cout << "   ✓ Follower driver: "
              << (drivers.follower ? "available" : "unavailable") << "\n";

    // Read joint positions once
    std::cout << "\n3. Reading joint positions...\n";
    auto leader_output = drivers.leader->get_robot_output();
    auto follower_output = drivers.follower->get_robot_output();

    std::cout << "   Leader:   " << leader_output.joint.all.positions.size() << " joints\n";
    std::cout << "   Follower: " << follower_output.joint.all.positions.size() << " joints\n";

    // Cleanup
    trossen::hw::ActiveHardwareRegistry::clear();
    std::cout << "\n✓ Demo complete!\n\n";
  } catch (const std::exception& e) {
    std::cerr << "   ✗ Error: " << e.what() << "\n";
    std::cerr << "\nNote: Ensure arms are connected at:\n";
    std::cerr << "  Leader:   " << leader_ip << "\n";
    std::cerr << "  Follower: " << follower_ip << "\n\n";
    return 1;
  }

  return 0;
}
