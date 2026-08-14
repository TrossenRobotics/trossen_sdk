/**
 * @file hardware_registry.cpp
 * @brief Implementation of the hardware registry
 */

#include "trossen_sdk/hw/hardware_registry.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace trossen::hw {

std::map<std::string, HardwareRegistry::FactoryFunc>& HardwareRegistry::get_registry() {
  // This static variable is the singleton registry map and is initialized on first use
  static std::map<std::string, FactoryFunc> registry;
  return registry;
}

std::mutex& HardwareRegistry::get_mutex() {
  // In practice this map is written only by REGISTER_HARDWARE during static
  // initialization and read-only afterwards, so concurrent create() lookups
  // would be safe without a lock. Guarded anyway: it costs a couple of
  // microseconds per create, and it removes the need for a comment asserting an
  // invariant that any future dynamic registration would quietly break.
  static std::mutex mutex;
  return mutex;
}

void HardwareRegistry::register_hardware(const std::string& type, FactoryFunc factory) {
  std::lock_guard<std::mutex> guard(get_mutex());
  auto& registry = get_registry();
  if (registry.find(type) != registry.end()) {
    throw std::runtime_error("Hardware type '" + type + "' is already registered");
  }
  registry[type] = std::move(factory);
}

std::shared_ptr<HardwareComponent> HardwareRegistry::create(
  const std::string& type,
  const std::string& identifier,
  const nlohmann::json& config,
  bool mark_active)
{
  // Copy the factory OUT of the map, then release the lock before using it.
  //
  // Everything below this block -- constructing the component and configuring
  // it -- is the entire multi-second cost of a bring-up: an arm's TCP handshake,
  // a ZED's open, a swerve base's mechanical homing. Holding a global mutex
  // across it would serialise every concurrent create and make parallel
  // bring-up pointless. The lock exists to protect the map, nothing more.
  FactoryFunc factory;
  {
    std::lock_guard<std::mutex> guard(get_mutex());
    auto& registry = get_registry();
    auto it = registry.find(type);
    if (it == registry.end()) {
      throw std::runtime_error("Unsupported hardware type: '" + type + "'");
    }
    factory = it->second;
  }

  // Create hardware instance with identifier
  auto hardware = factory(identifier);

  // Configure the hardware. Unlocked, deliberately -- and note configure() may
  // itself reach into the ActiveHardwareRegistry to resolve components created
  // earlier (glide_arm_input looks up its handle arms), which is only safe
  // because no lock is held here.
  try {
    hardware->configure(config);
  } catch (const std::exception& e) {
    throw std::runtime_error(
      "Failed to configure hardware '" + identifier + "' of type '" + type + "': " +
      e.what());
  }

  // Optionally register in active hardware registry. Takes that registry's own
  // lock, which is why this happens after configure() rather than around it.
  if (mark_active) {
    ActiveHardwareRegistry::register_active(identifier, hardware);
  }

  return hardware;
}

bool HardwareRegistry::is_registered(const std::string& type) {
  std::lock_guard<std::mutex> guard(get_mutex());
  const auto& registry = get_registry();
  return registry.find(type) != registry.end();
}

std::vector<std::string> HardwareRegistry::get_registered_types() {
  std::lock_guard<std::mutex> guard(get_mutex());
  auto& registry = get_registry();
  std::vector<std::string> types;
  types.reserve(registry.size());
  for (const auto& pair : registry) {
    types.push_back(pair.first);
  }
  return types;
}

}  // namespace trossen::hw
