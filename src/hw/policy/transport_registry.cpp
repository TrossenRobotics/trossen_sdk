/**
 * @file transport_registry.cpp
 * @brief Implementation of TransportRegistry.
 */

#include "trossen_sdk/hw/policy/transport_registry.hpp"

#include <stdexcept>
#include <utility>

#include "trossen_sdk/hw/policy/policy_transport.hpp"

namespace trossen::hw::policy {

std::map<std::string, TransportRegistry::FactoryFunc> & TransportRegistry::get_registry()
{
  static std::map<std::string, FactoryFunc> registry;
  return registry;
}

std::mutex & TransportRegistry::get_mutex()
{
  static std::mutex mutex;
  return mutex;
}

void TransportRegistry::register_factory(const std::string & name, FactoryFunc factory)
{
  if (name.empty()) {
    throw std::runtime_error("Transport name must be a non-empty string");
  }
  if (!factory) {
    throw std::runtime_error("Transport factory must be callable for name: " + name);
  }
  std::lock_guard<std::mutex> lock(get_mutex());
  auto & registry = get_registry();
  if (registry.find(name) != registry.end()) {
    throw std::runtime_error("Transport name already registered: " + name);
  }
  registry.emplace(name, std::move(factory));
}

std::unique_ptr<PolicyTransport> TransportRegistry::create(
  const std::string & name,
  const std::string & id,
  const std::string & server_url,
  const nlohmann::json & transport_config)
{
  FactoryFunc factory;
  {
    std::lock_guard<std::mutex> lock(get_mutex());
    auto & registry = get_registry();
    const auto it = registry.find(name);
    if (it == registry.end()) {
      throw std::runtime_error("Transport not registered: '" + name + "'");
    }
    factory = it->second;
  }
  std::unique_ptr<PolicyTransport> transport;
  try {
    transport = factory(id, server_url, transport_config);
  } catch (const std::exception & e) {
    throw std::runtime_error("Transport factory threw for '" + name + "': " + e.what());
  } catch (...) {
    throw std::runtime_error("Transport factory threw a non-std::exception for: " + name);
  }
  if (!transport) {
    throw std::runtime_error("Transport factory returned nullptr for: " + name);
  }
  return transport;
}

bool TransportRegistry::is_registered(const std::string & name)
{
  std::lock_guard<std::mutex> lock(get_mutex());
  return get_registry().count(name) > 0;
}

std::vector<std::string> TransportRegistry::get_registered_names()
{
  std::lock_guard<std::mutex> lock(get_mutex());
  std::vector<std::string> names;
  names.reserve(get_registry().size());
  for (const auto & [name, factory] : get_registry()) {
    names.push_back(name);
  }
  return names;
}

}  // namespace trossen::hw::policy
