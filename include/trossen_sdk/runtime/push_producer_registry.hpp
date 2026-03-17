/**
 * @file push_producer_registry.hpp
 * @brief Factory registry for push producer types
 */

#ifndef TROSSEN_SDK__RUNTIME__PUSH_PRODUCER_REGISTRY_HPP_
#define TROSSEN_SDK__RUNTIME__PUSH_PRODUCER_REGISTRY_HPP_

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

namespace trossen::runtime {

/**
 * @brief Static registry for push producer factory functions
 *
 * Push producer implementations register themselves at static initialization time.
 *
 * Key Design Decision: Factory does NOT take polling period parameter. Push producers manage
 * their own threads and timing internally. This maintains separation of concerns.
 */
class PushProducerRegistry {
public:
  /**
   * @brief Factory function signature for creating push producer instances
   *
   * @param hardware Hardware component pointer (may be nullptr for mock producers)
   * @param config JSON configuration object specific to the producer type
   * @return Shared pointer to created push producer instance
   */
  using FactoryFunc = std::function<std::shared_ptr<hw::PushProducer>(
    std::shared_ptr<hw::HardwareComponent> hardware,
    const nlohmann::json& config)>;

  /**
   * @brief Register a push producer factory function
   *
   * @param type Push producer type string (e.g., "realsense_depth")
   * @param factory Factory function that creates push producer instances
   *
   * @throws std::runtime_error if type is already registered
   *
   * @note This should be called during static initialization, typically using
   *       REGISTER_PUSH_PRODUCER macro in the producer implementation file.
   */
  static void register_producer(const std::string& type, FactoryFunc factory);

  /**
   * @brief Create a push producer instance by type
   *
   * @param type Push producer type string
   * @param hardware Hardware component
   * @param config Producer-specific configuration
   *
   * @return Shared pointer to created push producer instance
   *
   * @throws std::runtime_error if type is not registered
   *
   * @note The hardware parameter may be nullptr for producers that do not require hardware
   *       (e.g. mock producers).
   */
  static std::shared_ptr<hw::PushProducer> create(
    const std::string& type,
    std::shared_ptr<hw::HardwareComponent> hardware,
    const nlohmann::json& config);

  /**
   * @brief Check if a push producer type is registered
   *
   * @param type Push producer type string
   * @return true if the type is registered, false otherwise
   */
  static bool is_registered(const std::string& type);

  /**
   * @brief Get list of all registered push producer types
   *
   * @return Vector of registered type strings
   */
  static std::vector<std::string> get_registered_types();

private:
  static std::map<std::string, FactoryFunc>& get_registry();
};

/**
 * @brief Macro to register a push producer type with the PushProducerRegistry
 *
 * Creates a static registrar object that registers the push producer factory function during
 * static initialization. Use this in your push producer implementation (.cpp) file.
 *
 * @param ClassName The push producer class name (e.g., RealsenseDepthProducer)
 * @param TypeString The type string for this producer (e.g., "realsense_depth")
 *
 * Example usage:
 * @code
 * // In realsense_depth_producer.cpp namespace trossen::hw::depth {
 * REGISTER_PUSH_PRODUCER(RealsenseDepthProducer, "realsense_depth")
 * }
 * @endcode
 */
#define REGISTER_PUSH_PRODUCER(ClassName, TypeString)                                         \
  namespace {                                                                                  \
  struct ClassName##PushRegistrar {                                                            \
    ClassName##PushRegistrar() {                                                               \
      ::trossen::runtime::PushProducerRegistry::register_producer(                            \
        TypeString,                                                                            \
        [](std::shared_ptr<::trossen::hw::HardwareComponent> hw,                              \
           const nlohmann::json& cfg)                                                          \
             -> std::shared_ptr<::trossen::hw::PushProducer> {                                \
          return std::make_shared<ClassName>(hw, cfg);                                         \
        });                                                                                    \
    }                                                                                          \
  };                                                                                           \
  static ClassName##PushRegistrar ClassName##_push_registrar_instance;                        \
  }  /* anonymous namespace */

}  // namespace trossen::runtime

#endif  // TROSSEN_SDK__RUNTIME__PUSH_PRODUCER_REGISTRY_HPP_
