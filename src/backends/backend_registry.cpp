/**
 * @file backend_registry.cpp
 * @brief Implementation of the backend registry.
 */

#include "trossen_sdk/io/backend_registry.hpp"

namespace trossen::io {

std::map<std::string, BackendRegistry::FactoryFunc>& BackendRegistry::get_registry() {
  // This static variable is the singleton registry map and is initialized on first use.
  static std::map<std::string, FactoryFunc> registry;
  return registry;
}

void BackendRegistry::register_backend(const std::string& type, FactoryFunc factory) {
  auto& registry = get_registry();
  if (registry.find(type) != registry.end()) {
    throw std::runtime_error("Backend type '" + type + "' is already registered");
  }
  registry[type] = factory;
}

std::shared_ptr<Backend> BackendRegistry::create(
  const std::string& type,
  const ProducerMetadataList& producer_metadatas)
{
  auto& registry = get_registry();
  auto it = registry.find(type);
  if (it == registry.end()) {
    throw std::runtime_error("Unsupported backend type: '" + type + "'");
  }
  return it->second(producer_metadatas);
}

bool BackendRegistry::is_registered(const std::string& type) {
  auto& registry = get_registry();
  return registry.find(type) != registry.end();
}

}  // namespace trossen::io
