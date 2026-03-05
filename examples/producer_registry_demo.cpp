/**
 * @file producer_registry_demo.cpp
 * @brief Demo of the ProducerRegistry system with hardware and mock producers
 */

#include <iostream>

#include "nlohmann/json.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"

// Mock hardware component for demo
namespace demo {

class MockArmComponent : public trossen::hw::HardwareComponent {
public:
  explicit MockArmComponent(const std::string& identifier)
    : HardwareComponent(identifier) {}

  void configure(const nlohmann::json& config) override {
    num_joints_ = config.value("num_joints", 6);
    std::cout << "  [ok] Configured " << get_identifier()
              << " (" << num_joints_ << " joints)\n";
  }

  std::string get_type() const override { return "mock_arm"; }
  nlohmann::json get_info() const override {
    return {{"type", "mock_arm"}, {"num_joints", num_joints_}};
  }
  int get_num_joints() const { return num_joints_; }

private:
  int num_joints_ = 6;
};

// Mock producer that requires hardware
class MockArmProducer : public trossen::hw::PolledProducer {
public:
  MockArmProducer(std::shared_ptr<MockArmComponent> hw, const nlohmann::json& config)
    : hardware_(hw) {
    stream_id_ = config.value("stream_id", "joint_states");
    std::cout << "  [ok] Created MockArmProducer for " << hardware_->get_identifier()
              << " (stream: " << stream_id_ << ")\n";
  }

  void poll(const std::function<void(std::shared_ptr<trossen::data::RecordBase>)>& emit) override {
    // Mock implementation - would normally read from hardware
    auto record = std::make_shared<trossen::data::RecordBase>();
    record->id = stream_id_;
    emit(record);
  }

  std::shared_ptr<ProducerMetadata> metadata() const override {
    auto meta = std::make_shared<ProducerMetadata>();
    meta->type = "mock_arm";
    meta->id = stream_id_;
    meta->name = hardware_->get_identifier();
    meta->description = "Mock arm producer";
    return meta;
  }

private:
  std::shared_ptr<MockArmComponent> hardware_;
  std::string stream_id_;
};

// Mock producer that doesn't need hardware
class SyntheticProducer : public trossen::hw::PolledProducer {
public:
  explicit SyntheticProducer(const nlohmann::json& config) {
    stream_id_ = config.value("stream_id", "synthetic");
    frequency_ = config.value("frequency", 1.0);
    std::cout << "  [ok] Created SyntheticProducer (stream: " << stream_id_
              << ", freq: " << frequency_ << " Hz)\n";
  }

  void poll(const std::function<void(std::shared_ptr<trossen::data::RecordBase>)>& emit) override {
    // Mock implementation - generates synthetic data
    auto record = std::make_shared<trossen::data::RecordBase>();
    record->id = stream_id_;
    emit(record);
  }

  std::shared_ptr<ProducerMetadata> metadata() const override {
    auto meta = std::make_shared<ProducerMetadata>();
    meta->type = "synthetic";
    meta->id = stream_id_;
    meta->name = "Synthetic Producer";
    meta->description = "Generates synthetic data";
    return meta;
  }

private:
  std::string stream_id_;
  double frequency_;
};

// Register hardware and producers
REGISTER_HARDWARE(MockArmComponent, "mock_arm")

}  // namespace demo

// Register producers with custom factory logic
namespace demo {

// Hardware-based producer registration
namespace {
struct MockArmProducerRegistrar {
  MockArmProducerRegistrar() {
    trossen::runtime::ProducerRegistry::register_producer(
      "mock_arm",
      [](std::shared_ptr<trossen::hw::HardwareComponent> hw,
         const nlohmann::json& cfg) -> std::shared_ptr<trossen::hw::PolledProducer> {
        // Require hardware
        if (!hw) {
          throw std::invalid_argument("MockArmProducer requires hardware component");
        }

        // Type check
        auto arm_hw = std::dynamic_pointer_cast<MockArmComponent>(hw);
        if (!arm_hw) {
          throw std::invalid_argument(
            "MockArmProducer requires MockArmComponent, got: " + hw->get_type());
        }

        return std::make_shared<MockArmProducer>(arm_hw, cfg);
      });
  }
};
static MockArmProducerRegistrar mock_arm_producer_registrar;

// Synthetic producer registration (no hardware needed)
struct SyntheticProducerRegistrar {
  SyntheticProducerRegistrar() {
    trossen::runtime::ProducerRegistry::register_producer(
      "synthetic",
      [](std::shared_ptr<trossen::hw::HardwareComponent> hw,
         const nlohmann::json& cfg) -> std::shared_ptr<trossen::hw::PolledProducer> {
        // Hardware not required (ignored if provided)
        return std::make_shared<SyntheticProducer>(cfg);
      });
  }
};
static SyntheticProducerRegistrar synthetic_producer_registrar;

}  // anonymous namespace
}  // namespace demo

int main() {
  std::cout << "\nProducer Registry Demo\n";
  std::cout << "======================\n\n";

  // 1. Create and register hardware
  std::cout << "Step 1: Create hardware\n";
  auto left_arm = trossen::hw::HardwareRegistry::create(
    "mock_arm", "left_arm", {{"num_joints", 7}});
  // Note: Hardware auto-registered in ActiveHardwareRegistry by create()

  // 2. Create hardware-based producer
  std::cout << "\nStep 2: Create hardware-based producer\n";
  auto hw_left = trossen::hw::ActiveHardwareRegistry::get("left_arm");
  auto arm_producer = trossen::runtime::ProducerRegistry::create(
    "mock_arm", hw_left, {{"stream_id", "left_arm/joints"}});

  // 3. Create synthetic producer (no hardware)
  std::cout << "\nStep 3: Create synthetic producer\n";
  auto synth_producer = trossen::runtime::ProducerRegistry::create(
    "synthetic", nullptr, {{"stream_id", "clock"}, {"frequency", 10.0}});

  // 4. List registered types
  std::cout << "\nStep 4: Registry info\n";
  std::cout << "  Hardware types: ";
  for (const auto& type : trossen::hw::HardwareRegistry::get_registered_types()) {
    std::cout << type << " ";
  }
  std::cout << "\n  Producer types: ";
  for (const auto& type : trossen::runtime::ProducerRegistry::get_registered_types()) {
    std::cout << type << " ";
  }
  std::cout << "\n";

  // 5. Test producers
  std::cout << "\nStep 5: Test producers\n";
  int emit_count = 0;
  auto emit_callback = [&](std::shared_ptr<trossen::data::RecordBase> rec) {
    emit_count++;
  };

  arm_producer->poll(emit_callback);
  synth_producer->poll(emit_callback);
  std::cout << "  [ok] Emitted " << emit_count << " records\n";

  // Cleanup
  trossen::hw::ActiveHardwareRegistry::clear();

  std::cout << "\n[ok] Demo complete!\n\n";
  return 0;
}
