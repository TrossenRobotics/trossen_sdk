/**
 * @file hardware_registry.hpp
 * @brief Factory registry for hardware component types
 */

#ifndef TROSSEN_SDK__HW__HARDWARE_REGISTRY_HPP_
#define TROSSEN_SDK__HW__HARDWARE_REGISTRY_HPP_

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "trossen_sdk/hw/hardware_component.hpp"

namespace trossen::hw {

/**
 * @brief Static registry for hardware component factory functions
 *
 * Hardware implementations register themselves at static initialization time.
 */
class HardwareRegistry {
public:
  /// @brief Factory function signature for creating hardware instances
  /// @param identifier Hardware component identifier
  using FactoryFunc = std::function<std::shared_ptr<HardwareComponent>(const std::string&)>;

  /**
   * @brief Register a hardware factory function
   *
   * @param type Hardware type string (e.g., "trossen_arm", "opencv_camera")
   * @param factory Factory function that creates hardware instances
   *
   * @throws std::runtime_error if type is already registered
   *
   * @note This should be called during static initialization, typically using a static registrar
   *       object in the hardware implementation file.
   */
  static void register_hardware(const std::string& type, FactoryFunc factory);

  /**
   * @brief Create a hardware component instance by type
   *
   * @param type Hardware type string
   * @param identifier Unique identifier for this hardware instance
   * @param config JSON configuration for the hardware
   *
   * @return Shared pointer to created hardware instance
   *
   * @throws std::runtime_error if type is not registered
   * @throws std::runtime_error if configuration fails
   */
  static std::shared_ptr<HardwareComponent> create(
    const std::string& type,
    const std::string& identifier,
    const nlohmann::json& config);

  /**
   * @brief Check if a hardware type is registered
   *
   * @param type Hardware type string
   * @return true if the type is registered, false otherwise
   */
  static bool is_registered(const std::string& type);

  /**
   * @brief Get list of all registered hardware types
   *
   * @return Vector of registered type strings
   */
  static std::vector<std::string> get_registered_types();

private:
  /**
   * @brief Get the singleton registry map
   *
   * @return Reference to the static registry map
   *
   * @note Using a function-local static ensures proper initialization order
   */
  static std::map<std::string, FactoryFunc>& get_registry();
};

/**
 * @brief Macro to register a hardware type with the HardwareRegistry
 *
 * Creates a static registrar object that registers the hardware factory
 * function during static initialization. Use this in your hardware
 * implementation (.cpp) file.
 *
 * @param ClassName The hardware class name (e.g., TrossenArmComponent)
 * @param TypeString The type string for this hardware (e.g., "trossen_arm")
 *
 * Example usage:
 * @code
 * // In trossen_arm_component.cpp
 * namespace trossen::hw::arm {
 * REGISTER_HARDWARE(TrossenArmComponent, "trossen_arm")
 * }
 * @endcode
 */
#define REGISTER_HARDWARE(ClassName, TypeString)                                         \
  namespace {                                                                            \
  struct ClassName##Registrar {                                                          \
    ClassName##Registrar() {                                                             \
      ::trossen::hw::HardwareRegistry::register_hardware(                                \
        TypeString,                                                                      \
        [](const std::string& id) -> std::shared_ptr<::trossen::hw::HardwareComponent> { \
          return std::make_shared<ClassName>(id);                                        \
        });                                                                              \
    }                                                                                    \
  };                                                                                     \
  static ClassName##Registrar ClassName##_registrar_instance;                           \
  }  /* anonymous namespace */

}  // namespace trossen::hw

#endif  // TROSSEN_SDK__HW__HARDWARE_REGISTRY_HPP_
