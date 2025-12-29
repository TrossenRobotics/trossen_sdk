/**
 * @file producer_registry.hpp
 * @brief Factory registry for producer types
 */

#ifndef TROSSEN_SDK__RUNTIME__PRODUCER_REGISTRY_HPP_
#define TROSSEN_SDK__RUNTIME__PRODUCER_REGISTRY_HPP_

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
 * @brief Static registry for producer factory functions
 *
 * Producer implementations register themselves at static initialization time.
 *
 * Key Design Decision: Factory does NOT take polling period parameter. Period is a scheduling
 * concern handled by SessionManager, not a creation concern. This maintains separation of concerns
 * and matches existing SessionManager::add_producer(producer, period) API.
 */
class ProducerRegistry {
public:
  /**
   * @brief Factory function signature for creating producer instances
   *
   * @param hardware Hardware component pointer (may be nullptr for mock producers)
   * @param config JSON configuration object specific to the producer type
   * @return Shared pointer to created producer instance
   */
  using FactoryFunc = std::function<std::shared_ptr<hw::PolledProducer>(
    std::shared_ptr<hw::HardwareComponent> hardware,
    const nlohmann::json& config)>;

  /**
   * @brief Register a producer factory function
   *
   * @param type Producer type string (e.g., "trossen_arm", "opencv_camera")
   * @param factory Factory function that creates producer instances
   *
   * @throws std::runtime_error if type is already registered
   *
   * @note This should be called during static initialization, typically using REGISTER_PRODUCER
   *       macro in the producer implementation file.
   */
  static void register_producer(const std::string& type, FactoryFunc factory);

  /**
   * @brief Create a producer instance by type
   *
   * @param type Producer type string
   * @param hardware Hardware component
   * @param config Producer-specific configuration
   *
   * @return Shared pointer to created producer instance
   *
   * @throws std::runtime_error if type is not registered
   *
   * @note The hardware parameter may be nullptr for producers that do not require hardware (e.g.
   * mock producers).
   */
  static std::shared_ptr<hw::PolledProducer> create(
    const std::string& type,
    std::shared_ptr<hw::HardwareComponent> hardware,
    const nlohmann::json& config);

  /**
   * @brief Check if a producer type is registered
   *
   * @param type Producer type string
   * @return true if the type is registered, false otherwise
   */
  static bool is_registered(const std::string& type);

  /**
   * @brief Get list of all registered producer types
   *
   * @return Vector of registered type strings
   */
  static std::vector<std::string> get_registered_types();

private:
  static std::map<std::string, FactoryFunc>& get_registry();
};

/**
 * @brief Macro to register a producer type with the ProducerRegistry
 *
 * Creates a static registrar object that registers the producer factory function during static
 * initialization. Use this in your producer implementation (.cpp) file.
 *
 * @param ClassName The producer class name (e.g., TrossenArmProducer)
 * @param TypeString The type string for this producer (e.g., "trossen_arm")
 *
 * Example usage:
 * @code
 * // In trossen_arm_producer.cpp namespace trossen::hw::arm {
 * REGISTER_PRODUCER(TrossenArmProducer, "trossen_arm")
 * }
 * @endcode
 */
#define REGISTER_PRODUCER(ClassName, TypeString)                                         \
  namespace {                                                                            \
  struct ClassName##Registrar {                                                          \
    ClassName##Registrar() {                                                             \
      ::trossen::runtime::ProducerRegistry::register_producer(                           \
        TypeString,                                                                      \
        [](std::shared_ptr<::trossen::hw::HardwareComponent> hw,                        \
           const nlohmann::json& cfg)                                                    \
             -> std::shared_ptr<::trossen::hw::PolledProducer> {                        \
          return std::make_shared<ClassName>(hw, cfg);                                   \
        });                                                                              \
    }                                                                                    \
  };                                                                                     \
  static ClassName##Registrar ClassName##_registrar_instance;                           \
  }  /* anonymous namespace */

}  // namespace trossen::runtime

#endif  // TROSSEN_SDK__RUNTIME__PRODUCER_REGISTRY_HPP_
