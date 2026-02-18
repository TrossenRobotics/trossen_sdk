/**
 * @file teleop_arm_component_demo.cpp
 * @brief Minimal demo showcasing TeleopArmComponent with leader-follower configuration
 *
 * This demo shows how to:
 *   1. Create a TeleopArmComponent from configuration
 *   2. Access leader and follower drivers
 *   3. Read joint positions
 *
 * USAGE WITH TELEOP ARM PRODUCER:
 * ================================
 * To create a TeleopTrossenArmProducer that records both leader and follower data
 * using the ProducerRegistry pattern:
 *
 * @code
 * // 1. Create a TeleopArmComponent (contains both leader and follower)
 * auto teleop_component = trossen::hw::HardwareRegistry::create(
 *   "teleop_arm", "teleop_arm_pair",
 *   {{"leader", {{"ip_address", "192.168.1.2"},
 *                {"model", "wxai_v0"},
 *                {"end_effector", "wxai_v0_leader"}}},
 *    {"follower", {{"ip_address", "192.168.1.4"},
 *                  {"model", "wxai_v0"},
 *                  {"end_effector", "wxai_v0_follower"}}}});
 *
 * // 2. Create the teleop producer via registry
 * const float arm_rate_hz = 30.0f;
 * auto joint_period = std::chrono::milliseconds(static_cast<int>(1000.0f / arm_rate_hz));
 * nlohmann::json prod_cfg = {
 *   {"stream_id", "teleop_arm/states"},
 *   {"use_device_time", false}
 * };
 * auto producer = trossen::runtime::ProducerRegistry::create(
 *   "teleop_arm", teleop_component, prod_cfg);
 *
 * // 3. Add producer to session manager
 * mgr.add_producer(producer, joint_period);
 * @endcode
 */

#include <chrono>
#include <iostream>
#include <thread>

#include "nlohmann/json.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/arm/teleop_arm_component.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"

int main(int argc, char** argv) {
  // Parse command line arguments
  std::string leader_ip = "192.168.1.2";
  std::string follower_ip = "192.168.1.4";
  bool demo_producer = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--leader-ip" && i + 1 < argc) {
      leader_ip = argv[++i];
    } else if (arg == "--follower-ip" && i + 1 < argc) {
      follower_ip = argv[++i];
    } else if (arg == "--demo-producer") {
      demo_producer = true;
    } else if (arg == "--help") {
      std::cout << "Usage: " << argv[0] << " [OPTIONS]\n";
      std::cout << "Options:\n";
      std::cout << "  --leader-ip <IP>     Leader arm IP address (default: 192.168.1.2)\n";
      std::cout << "  --follower-ip <IP>   Follower arm IP address (default: 192.168.1.4)\n";
      std::cout << "  --demo-producer      Also demonstrate TeleopTrossenArmProducer\n";
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

    // --------------------------------------------------------------------------
    // OPTIONAL: Demonstrate TeleopTrossenArmProducer
    // --------------------------------------------------------------------------
    if (demo_producer) {
      std::cout << "\n4. Creating TeleopTrossenArmProducer via registry...\n";

      // Create the teleop producer using the existing TeleopArmComponent
      const float arm_rate_hz = 30.0f;
      auto joint_period = std::chrono::milliseconds(
        static_cast<int>(1000.0f / arm_rate_hz));

      nlohmann::json prod_cfg = {
        {"stream_id", "teleop_arm/states"},
        {"use_device_time", false}
      };

      // Reuse the existing teleop_component from step 1
      auto producer = trossen::runtime::ProducerRegistry::create(
        "teleop_arm", teleop_component, prod_cfg);
      std::cout << "   ✓ Created TeleopTrossenArmProducer from existing TeleopArmComponent\n";

      // Test polling the producer a few times
      std::cout << "\n5. Testing producer polling (3 samples)...\n";
      int sample_count = 0;
      auto emit_callback = [&sample_count](std::shared_ptr<trossen::data::RecordBase> rec) {
        sample_count++;
        std::cout << "   ✓ Sample " << sample_count << " - Stream ID: " << rec->id
                  << ", Seq: " << rec->seq << "\n";
      };

      for (int i = 0; i < 3; ++i) {
        producer->poll(emit_callback);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      std::cout << "   Total samples: " << sample_count << "\n";
      std::cout << "\n   NOTE: In a real application, add the producer to a SessionManager:\n";
      std::cout << "         mgr.add_producer(producer, joint_period);\n";
    }

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
