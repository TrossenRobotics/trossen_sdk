/**
 * @file active_hardware_registry.cpp
 * @brief Implementation of the active hardware registry
 */

#include "trossen_sdk/hw/active_hardware_registry.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace trossen::hw {

std::map<std::string, std::shared_ptr<HardwareComponent>>&
ActiveHardwareRegistry::get_registry() {
  // This static variable is the singleton registry map and is initialized on first use
  static std::map<std::string, std::shared_ptr<HardwareComponent>> registry;
  return registry;
}

std::mutex& ActiveHardwareRegistry::get_mutex() {
  // Guards every access to the map above, which is shared mutable state reached
  // from several threads: hardware bring-up may open devices concurrently, and
  // each component's configure() can resolve earlier components out of this map
  // (glide_arm_input looks up its handle arms that way).
  //
  // Scope is deliberately narrow -- MAP OPERATIONS ONLY, measured in
  // microseconds. It must never be held across a device open: configure() is
  // the entire multi-second cost of a bring-up, and serialising it here would
  // undo concurrent bring-up completely. HardwareRegistry::create() already
  // calls configure() before register_active(), so the two do not overlap.
  static std::mutex mutex;
  return mutex;
}

void ActiveHardwareRegistry::register_active(
  const std::string& id,
  std::shared_ptr<HardwareComponent> component)
{
  if (!component) {
    throw std::runtime_error("Cannot register null hardware component with id: " + id);
  }

  std::lock_guard<std::mutex> guard(get_mutex());
  auto& registry = get_registry();

  // Under the lock so the check and the insert cannot interleave with another
  // thread doing the same: unlocked, two concurrent creates with one id would
  // both see it absent and the second would silently replace the first,
  // dropping a live device that nothing else owns.
  if (registry.find(id) != registry.end()) {
    throw std::runtime_error("Hardware component already registered with id: " + id);
  }

  registry[id] = std::move(component);
}

std::shared_ptr<HardwareComponent> ActiveHardwareRegistry::get(const std::string& id) {
  std::lock_guard<std::mutex> guard(get_mutex());
  auto& registry = get_registry();
  auto it = registry.find(id);
  return (it != registry.end()) ? it->second : nullptr;
}

std::map<std::string, std::shared_ptr<HardwareComponent>>
ActiveHardwareRegistry::get_all() {
  std::lock_guard<std::mutex> guard(get_mutex());
  // Returns a copy, so the caller iterates its own snapshot rather than the
  // shared map. Copying the shared_ptrs also keeps every component alive for as
  // long as the caller holds the result, even if the registry is cleared.
  return get_registry();
}

std::vector<std::string> ActiveHardwareRegistry::get_ids() {
  std::lock_guard<std::mutex> guard(get_mutex());
  auto& registry = get_registry();
  std::vector<std::string> ids;
  ids.reserve(registry.size());
  for (const auto& [id, _] : registry) {
    ids.push_back(id);
  }
  return ids;
}

bool ActiveHardwareRegistry::is_registered(const std::string& id) {
  std::lock_guard<std::mutex> guard(get_mutex());
  const auto& registry = get_registry();
  return registry.find(id) != registry.end();
}

bool ActiveHardwareRegistry::unregister(const std::string& id) {
  // The removed component is moved OUT of the map under the lock and destroyed
  // after it is released. Destroying it in place would run its destructor while
  // holding the mutex, and a destructor may call back in here --
  // PolicyClient::~PolicyClient() unregisters each of its Faces -- which with a
  // non-recursive mutex is a self-deadlock. See clear() for the same reasoning.
  std::shared_ptr<HardwareComponent> removed;
  {
    std::lock_guard<std::mutex> guard(get_mutex());
    auto& registry = get_registry();
    auto it = registry.find(id);
    if (it == registry.end()) {
      return false;
    }
    removed = std::move(it->second);
    registry.erase(it);
  }
  return true;
}

void ActiveHardwareRegistry::clear() {
  // Swap the map out under the lock, then let the local destruct with the mutex
  // released. This matters for two reasons, and the first is a hang rather than
  // a slowdown:
  //
  //   - a component destructor may re-enter this class
  //     (PolicyClient::~PolicyClient() unregisters its Faces), which would
  //     deadlock against a lock still held here;
  //   - these destructors are the slow driver teardown -- arms disconnecting,
  //     a ZED closing against CUDA -- and holding a global mutex across all of
  //     it would block every other registry user for the duration.
  std::map<std::string, std::shared_ptr<HardwareComponent>> doomed;
  {
    std::lock_guard<std::mutex> guard(get_mutex());
    doomed.swap(get_registry());
  }
  // `doomed` goes out of scope here, unlocked, running the teardown.
}

size_t ActiveHardwareRegistry::count() {
  std::lock_guard<std::mutex> guard(get_mutex());
  return get_registry().size();
}

}  // namespace trossen::hw
