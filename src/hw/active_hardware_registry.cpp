/**
 * @file active_hardware_registry.cpp
 * @brief Implementation of the active hardware registry
 */

#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include <stdexcept>

namespace trossen::hw {

std::map<std::string, std::shared_ptr<HardwareComponent>>&
ActiveHardwareRegistry::get_registry() {
  // This static variable is the singleton registry map and is initialized on first use
  static std::map<std::string, std::shared_ptr<HardwareComponent>> registry;
  return registry;
}

void ActiveHardwareRegistry::register_active(
  const std::string& id,
  std::shared_ptr<HardwareComponent> component)
{
  if (!component) {
    throw std::runtime_error("Cannot register null hardware component with id: " + id);
  }

  auto& registry = get_registry();

  if (registry.find(id) != registry.end()) {
    throw std::runtime_error("Hardware component already registered with id: " + id);
  }

  registry[id] = component;
}

std::shared_ptr<HardwareComponent> ActiveHardwareRegistry::get(const std::string& id) {
  auto& registry = get_registry();
  auto it = registry.find(id);
  return (it != registry.end()) ? it->second : nullptr;
}

std::map<std::string, std::shared_ptr<HardwareComponent>>
ActiveHardwareRegistry::get_all() {
  return get_registry();
}

std::vector<std::string> ActiveHardwareRegistry::get_ids() {
  auto& registry = get_registry();
  std::vector<std::string> ids;
  ids.reserve(registry.size());
  for (const auto& [id, _] : registry) {
    ids.push_back(id);
  }
  return ids;
}

bool ActiveHardwareRegistry::is_registered(const std::string& id) {
  return get_registry().find(id) != get_registry().end();
}

void ActiveHardwareRegistry::clear() {
  get_registry().clear();
}

size_t ActiveHardwareRegistry::count() {
  return get_registry().size();
}

}  // namespace trossen::hw
