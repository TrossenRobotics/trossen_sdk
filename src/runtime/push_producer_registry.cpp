/**
 * @file push_producer_registry.cpp
 * @brief Implementation of PushProducerRegistry
 */

#include <iostream>

#include "trossen_sdk/runtime/push_producer_registry.hpp"

namespace trossen::runtime {

std::map<std::string, PushProducerRegistry::FactoryFunc>&
PushProducerRegistry::get_registry() {
  static std::map<std::string, FactoryFunc> registry;
  return registry;
}

void PushProducerRegistry::register_producer(const std::string& type, FactoryFunc factory) {
  auto& registry = get_registry();

  if (registry.find(type) != registry.end()) {
    throw std::runtime_error("Push producer type already registered: " + type);
  }

  registry[type] = factory;
  std::cout << "Registered push producer type: " << type << std::endl;
}

std::shared_ptr<hw::PushProducer> PushProducerRegistry::create(
  const std::string& type,
  std::shared_ptr<hw::HardwareComponent> hardware,
  const nlohmann::json& config)
{
  auto& registry = get_registry();
  auto it = registry.find(type);

  if (it == registry.end()) {
    throw std::runtime_error("Push producer type not registered: " + type);
  }

  // Call factory function
  auto producer = it->second(hardware, config);

  if (!producer) {
    throw std::runtime_error("Failed to create push producer of type: " + type);
  }

  return producer;
}

bool PushProducerRegistry::is_registered(const std::string& type) {
  return get_registry().find(type) != get_registry().end();
}

std::vector<std::string> PushProducerRegistry::get_registered_types() {
  std::vector<std::string> types;
  for (const auto& [type, _] : get_registry()) {
    types.push_back(type);
  }
  return types;
}

}  // namespace trossen::runtime
