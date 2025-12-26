/**
 * @file hardware_registry.cpp
 * @brief Implementation of the hardware registry
 */

#include "trossen_sdk/hw/hardware_registry.hpp"

namespace trossen::hw {

std::map<std::string, HardwareRegistry::FactoryFunc>& HardwareRegistry::get_registry() {
  // This static variable is the singleton registry map and is initialized on first use
  static std::map<std::string, FactoryFunc> registry;
  return registry;
}

void HardwareRegistry::register_hardware(const std::string& type, FactoryFunc factory) {
  auto& registry = get_registry();
  if (registry.find(type) != registry.end()) {
    throw std::runtime_error("Hardware type '" + type + "' is already registered");
  }
  registry[type] = factory;
}

std::shared_ptr<HardwareComponent> HardwareRegistry::create(
  const std::string& type,
  const std::string& identifier,
  const nlohmann::json& config)
{
  auto& registry = get_registry();
  auto it = registry.find(type);
  if (it == registry.end()) {
    throw std::runtime_error("Unsupported hardware type: '" + type + "'");
  }

  // Create hardware instance with identifier
  auto hardware = it->second(identifier);

  // Configure the hardware
  try {
    hardware->configure(config);
  } catch (const std::exception& e) {
    throw std::runtime_error(
      "Failed to configure hardware '" + identifier + "' of type '" + type + "': " +
      e.what());
  }

  return hardware;
}

bool HardwareRegistry::is_registered(const std::string& type) {
  auto& registry = get_registry();
  return registry.find(type) != registry.end();
}

std::vector<std::string> HardwareRegistry::get_registered_types() {
  auto& registry = get_registry();
  std::vector<std::string> types;
  types.reserve(registry.size());
  for (const auto& pair : registry) {
    types.push_back(pair.first);
  }
  return types;
}

}  // namespace trossen::hw
