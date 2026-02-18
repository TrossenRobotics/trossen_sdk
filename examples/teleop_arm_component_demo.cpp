/**
 * @file teleop_arm_component_demo.cpp
 * @brief Demo showcasing TeleopArmComponent with Solo and Stationary producers
 *
 * Demonstrates:
 *   1. TeleopSoloArmProducer - Single leader-follower pair
 *   2. TeleopStationaryArmProducer - Bimanual (2 leaders + 2 followers)
 */

#include <chrono>
#include <iostream>
#include <thread>

#include "nlohmann/json.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"

int main() {
  std::cout << "\nTeleop Arm Component & Producer Demo\n";
  std::cout << "=====================================\n\n";

  try {
    // ==========================================================================
    // SOLO Producer Demo (Single Leader-Follower Pair)
    // ==========================================================================
    std::cout << "SOLO Producer Demo\n";
    std::cout << "------------------\n";

    // Create left arm pair
    std::cout << "1. Creating left arm TeleopArmComponent\n";
    nlohmann::json left_config = {
      {"leader",
       {{"ip_address", "192.168.1.2"}, {"model", "wxai_v0"}, {"end_effector", "wxai_v0_leader"}}},
      {"follower",
       {{"ip_address", "192.168.1.4"}, {"model", "wxai_v0"}, {"end_effector", "wxai_v0_follower"}}}
    };

    auto left_component = trossen::hw::HardwareRegistry::create(
      "teleop_arm", "left_pair", left_config);
    std::cout << "   ✓ Left arm pair created\n";

    // Create solo producer
    std::cout << "\n2. Creating TeleopSoloArmProducer\n";
    auto solo_producer = trossen::runtime::ProducerRegistry::create(
      "teleop_solo", left_component,
      {{"stream_id", "teleop_solo/states"}, {"use_device_time", false}});
    std::cout << "   ✓ TeleopSoloArmProducer created\n";

    // Poll solo producer
    std::cout << "\n3. Polling solo producer (3 samples)\n";
    int solo_count = 0;
    for (int i = 0; i < 3; ++i) {
      solo_producer->poll([&solo_count](auto rec) {
        std::cout << "   Sample " << ++solo_count << ": seq=" << rec->seq << "\n";
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ==========================================================================
    // STATIONARY Producer Demo (Bimanual: 2 Leaders + 2 Followers)
    // ==========================================================================
    std::cout << "\n\nSTATIONARY Producer Demo\n";
    std::cout << "------------------------\n";

    // Create right arm pair
    std::cout << "1. Creating right arm TeleopArmComponent\n";
    nlohmann::json right_config = {
      {"leader",
       {{"ip_address", "192.168.1.3"}, {"model", "wxai_v0"}, {"end_effector", "wxai_v0_leader"}}},
      {"follower",
       {{"ip_address", "192.168.1.5"}, {"model", "wxai_v0"}, {"end_effector", "wxai_v0_follower"}}}
    };

    auto right_component = trossen::hw::HardwareRegistry::create(
      "teleop_arm", "right_pair", right_config);
    std::cout << "   ✓ Right arm pair created\n";

    // Create stationary producer
    std::cout << "\n2. Creating TeleopStationaryArmProducer\n";
    auto stationary_producer = trossen::runtime::ProducerRegistry::create(
      "teleop_stationary", nullptr,
      {{"stream_id", "teleop_stationary/states"},
       {"use_device_time", false},
       {"left_id", "left_pair"},
       {"right_id", "right_pair"}});
    std::cout << "   ✓ TeleopStationaryArmProducer created\n";
    std::cout << "   ✓ Producer type: " << stationary_producer->metadata()->type << "\n";

    // Poll stationary producer
    std::cout << "\n3. Polling stationary producer (3 samples)\n";
    int stationary_count = 0;
    for (int i = 0; i < 3; ++i) {
      stationary_producer->poll([&stationary_count](auto rec) {
        std::cout << "   Sample " << ++stationary_count << ": seq=" << rec->seq << "\n";
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\n✓ Demo complete\n";

    trossen::hw::ActiveHardwareRegistry::clear();
  } catch (const std::exception& e) {
    std::cerr << "\n✗ Error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
