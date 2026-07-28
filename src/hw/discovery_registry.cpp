/**
 * @file discovery_registry.cpp
 * @brief Implementation of DiscoveryRegistry.
 */

#include "trossen_sdk/hw/discovery_registry.hpp"

#include <stdexcept>

namespace trossen::hw {

std::map<std::string, DiscoveryRegistry::DiscoveryFunc>&
DiscoveryRegistry::get_registry() {
  static std::map<std::string, DiscoveryFunc> registry;
  return registry;
}

void DiscoveryRegistry::register_discovery(const std::string& type, DiscoveryFunc fn) {
  auto& registry = get_registry();
  if (registry.find(type) != registry.end()) {
    throw std::runtime_error(
      "Hardware discovery for type '" + type + "' is already registered");
  }
  registry[type] = std::move(fn);
}

std::optional<std::vector<DiscoveredHardware>> DiscoveryRegistry::find(
  const std::string& type,
  const std::filesystem::path& output_dir)
{
  auto& registry = get_registry();
  auto it = registry.find(type);
  if (it == registry.end()) return std::nullopt;
  return it->second(output_dir);
}

bool DiscoveryRegistry::is_registered(const std::string& type) {
  auto& registry = get_registry();
  return registry.find(type) != registry.end();
}

std::vector<std::string> DiscoveryRegistry::supported_types() {
  auto& registry = get_registry();
  std::vector<std::string> types;
  types.reserve(registry.size());
  for (const auto& [type, _] : registry) types.push_back(type);
  return types;
}

}  // namespace trossen::hw
