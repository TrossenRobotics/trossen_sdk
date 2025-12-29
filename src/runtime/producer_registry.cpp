/**
 * @file producer_registry.cpp
 * @brief Implementation of ProducerRegistry
 */

#include <iostream>

#include "trossen_sdk/runtime/producer_registry.hpp"

namespace trossen::runtime {

std::map<std::string, ProducerRegistry::FactoryFunc>&
ProducerRegistry::get_registry() {
  static std::map<std::string, FactoryFunc> registry;
  return registry;
}

void ProducerRegistry::register_producer(const std::string& type, FactoryFunc factory) {
  auto& registry = get_registry();

  if (registry.find(type) != registry.end()) {
    throw std::runtime_error("Producer type already registered: " + type);
  }

  registry[type] = factory;
  std::cout << "Registered producer type: " << type << std::endl;
}

std::shared_ptr<hw::PolledProducer> ProducerRegistry::create(
  const std::string& type,
  std::shared_ptr<hw::HardwareComponent> hardware,
  const nlohmann::json& config)
{
  auto& registry = get_registry();
  auto it = registry.find(type);

  if (it == registry.end()) {
    throw std::runtime_error("Producer type not registered: " + type);
  }

  // Call factory function
  auto producer = it->second(hardware, config);

  if (!producer) {
    throw std::runtime_error("Failed to create producer of type: " + type);
  }

  return producer;
}

bool ProducerRegistry::is_registered(const std::string& type) {
  return get_registry().find(type) != get_registry().end();
}

std::vector<std::string> ProducerRegistry::get_registered_types() {
  std::vector<std::string> types;
  for (const auto& [type, _] : get_registry()) {
    types.push_back(type);
  }
  return types;
}

}  // namespace trossen::runtime
