/**
 * @file hardware_registry_demo.cpp
 * @brief Simple demo of the HardwareRegistry and ActiveHardwareRegistry systems
 */

#include <iostream>

#include "nlohmann/json.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/camera/realsense_camera_component.hpp"
#include "trossen_sdk/hw/camera/opencv_camera_component.hpp"

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

int main(int argc, char** argv) {
  // Parse command line arguments
  bool use_real_hardware = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--real") {
      use_real_hardware = true;
      break;
    }
  }

  std::cout << "\nHardware Registry Demo\n";
  std::cout << "======================\n\n";

  // 1. Create Hardware from configuration
  std::cout << "Creating hardware components from configuration...\n";

  // 1.1 Mock Arms hardware
  std::cout << "Mock Arms hardware from config:\n";
  nlohmann::json config = {{"num_joints", 7}};
  // Create left mock arm. It will be auto-registered.
  auto left_arm = trossen::hw::HardwareRegistry::create("mock_arm", "left_arm", config);
  // Create right mock arm but do not auto-register
  auto right_arm = trossen::hw::HardwareRegistry::create(
    "mock_arm", "right_arm", {{"num_joints", 6}}, false);

  std::shared_ptr<trossen::hw::HardwareComponent> real_arm;
  std::shared_ptr<trossen::hw::HardwareComponent> realsense_camera;
  std::shared_ptr<trossen::hw::HardwareComponent> opencv_camera;

  if (use_real_hardware) {
    // 1.2 Real Arms hardware
    std::cout << "\nReal Trossen Arms hardware from config:\n";
    nlohmann::json real_arm_config = {
      {"ip_address", "192.168.1.2"},
      {"model", "wxai_v0"},
      {"end_effector", "wxai_v0_leader"}
    };
    real_arm = trossen::hw::HardwareRegistry::create(
      "trossen_arm", "real_arm", real_arm_config);

    // 1.3 Realsense Camera hardware
    std::cout << "\nReal Realsense Camera hardware from config:\n";
    nlohmann::json camera_config = {
      {"serial_number", "218622274938"},
      {"width", 640},
      {"height", 480},
      {"fps", 30}
    };
    realsense_camera = trossen::hw::HardwareRegistry::create(
      "realsense_camera", "realsense_camera0", camera_config);

    // 1.4 OpenCV Camera hardware
    std::cout << "\nReal OpenCV Camera hardware from config:\n";
    nlohmann::json opencv_camera_config = {
      {"device_index", 0},
      {"width", 640},
      {"height", 480},
      {"fps", 30}
    };

    opencv_camera = trossen::hw::HardwareRegistry::create(
      "opencv_camera", "opencv_camera0", opencv_camera_config);
  }

  // 2. Register in active registry
  std::cout << "\nRegistering hardware:\n";
  // We only registered left_arm during creation; now register right_arm
  trossen::hw::ActiveHardwareRegistry::register_active("right_arm", right_arm);

  if (use_real_hardware) {
    trossen::hw::ActiveHardwareRegistry::register_active("real_arm", real_arm);
    trossen::hw::ActiveHardwareRegistry::register_active("realsense_camera0", realsense_camera);
    trossen::hw::ActiveHardwareRegistry::register_active("opencv_camera0", opencv_camera);
  }
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
