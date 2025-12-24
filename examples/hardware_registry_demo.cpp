/**
 * @file hardware_registry_demo.cpp
 * @brief Simple demo of the HardwareRegistry and ActiveHardwareRegistry systems
 */

#include <iostream>

#include "nlohmann/json.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"

// Mock hardware component for demo (doesn't require real devices)
namespace demo {

class MockArmComponent : public trossen::hw::HardwareComponent {
public:
  explicit MockArmComponent(const std::string& identifier)
    : HardwareComponent(identifier) {}

  void configure(const nlohmann::json& config) override {
    num_joints_ = config.value("num_joints", 6);
    std::cout << "  ✓ Configured '" << get_identifier()
              << "' (" << num_joints_ << " joints)\n";
  }

  std::string get_type() const override { return "mock_arm"; }
  nlohmann::json get_info() const override {
    return {{"type", "mock_arm"}, {"num_joints", num_joints_}};
  }
  int get_num_joints() const { return num_joints_; }

private:
  int num_joints_ = 6;
};

// Register with the hardware registry
REGISTER_HARDWARE(MockArmComponent, "mock_arm")

}  // namespace demo

int main() {
  std::cout << "\nHardware Registry Demo\n";
  std::cout << "======================\n\n";

  // 1. Create hardware from config
  std::cout << "Creating hardware from config:\n";
  nlohmann::json config = {{"num_joints", 7}};
  // Create left mock arm. It will be auto-registered.
  auto left_arm = trossen::hw::HardwareRegistry::create("mock_arm", "left_arm", config);
  // Create right mock arm but do not auto-register
  auto right_arm = trossen::hw::HardwareRegistry::create(
    "mock_arm", "right_arm", {{"num_joints", 6}}, false);

  // 2. Register in active registry
  std::cout << "\nRegistering hardware:\n";
  // We only registered left_arm during creation; now register right_arm
  trossen::hw::ActiveHardwareRegistry::register_active("right_arm", right_arm);
  std::cout << "  ✓ " << trossen::hw::ActiveHardwareRegistry::count()
            << " hardware components active\n";

  // 3. Retrieve and use hardware
  std::cout << "\nRetrieving hardware:\n";
  auto arm = trossen::hw::ActiveHardwareRegistry::get_as<demo::MockArmComponent>("left_arm");
  if (arm) {
    std::cout << "  ✓ Retrieved '" << arm->get_identifier()
              << "' with " << arm->get_num_joints() << " joints\n";
  }

  // 4. List all hardware
  std::cout << "\nActive hardware:\n";
  for (const auto& id : trossen::hw::ActiveHardwareRegistry::get_ids()) {
    auto hw = trossen::hw::ActiveHardwareRegistry::get(id);
    std::cout << "  • " << id << " (" << hw->get_type() << ")\n";
  }

  // 5. Cleanup
  trossen::hw::ActiveHardwareRegistry::clear();
  std::cout << "\n✓ Demo complete!\n\n";

  return 0;
}
