/**
 * @file observer_registry.cpp
 * @brief Implementation of ObserverRegistry.
 */

#include "trossen_sdk/observer/observer_registry.hpp"

namespace trossen::observer {

std::map<std::string, ObserverRegistry::FactoryFunc>&
ObserverRegistry::get_registry() {
  static std::map<std::string, FactoryFunc> registry;
  return registry;
}

std::mutex& ObserverRegistry::get_mutex() {
  static std::mutex mutex;
  return mutex;
}

void ObserverRegistry::register_observer(const std::string& type, FactoryFunc factory) {
  if (type.empty()) {
    throw std::runtime_error("Observer type must be a non-empty string");
  }
  if (!factory) {
    throw std::runtime_error("Observer factory must be callable for type: " + type);
  }
  std::lock_guard<std::mutex> lock(get_mutex());
  auto& registry = get_registry();
  if (registry.find(type) != registry.end()) {
    throw std::runtime_error("Observer type already registered: " + type);
  }
  registry.emplace(type, std::move(factory));
}

std::shared_ptr<ObserverBase> ObserverRegistry::create(
  const std::string& type,
  const nlohmann::json& config)
{
  FactoryFunc factory;
  {
    std::lock_guard<std::mutex> lock(get_mutex());
    auto& registry = get_registry();
    auto it = registry.find(type);
    if (it == registry.end()) {
      throw std::runtime_error("Observer type not registered: " + type);
    }
    factory = it->second;
  }
  std::shared_ptr<ObserverBase> obs;
  try {
    obs = factory(config);
  } catch (const std::exception& e) {
    throw std::runtime_error(
      "Observer factory threw for type '" + type + "': " + e.what());
  } catch (...) {
    throw std::runtime_error(
      "Observer factory threw a non-std::exception for type: " + type);
  }
  if (!obs) {
    throw std::runtime_error("Observer factory returned nullptr for type: " + type);
  }
  return obs;
}

bool ObserverRegistry::is_registered(const std::string& type) {
  std::lock_guard<std::mutex> lock(get_mutex());
  return get_registry().find(type) != get_registry().end();
}

std::vector<std::string> ObserverRegistry::get_registered_types() {
  std::lock_guard<std::mutex> lock(get_mutex());
  const auto& registry = get_registry();
  std::vector<std::string> types;
  types.reserve(registry.size());
  for (const auto& kv : registry) {
    types.push_back(kv.first);
  }
  return types;
}

}  // namespace trossen::observer
