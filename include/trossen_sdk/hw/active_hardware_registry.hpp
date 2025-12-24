/**
 * @file active_hardware_registry.hpp
 * @brief Registry for tracking active hardware component instances
 */

#ifndef TROSSEN_SDK__HW__ACTIVE_HARDWARE_REGISTRY_HPP_
#define TROSSEN_SDK__HW__ACTIVE_HARDWARE_REGISTRY_HPP_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "trossen_sdk/hw/hardware_component.hpp"

namespace trossen::hw {

/**
 * @brief Singleton registry for active hardware component instances
 *
 * Tracks hardware components that have been created and configured during
 * application lifetime. Provides named access for producer creation.
 *
 * Lifecycle is tied to application scope (not session scope). Hardware
 * persists across multiple recording episodes.
 */
class ActiveHardwareRegistry {
public:
  /**
   * @brief Register an active hardware component
   *
   * @param id Unique identifier for this hardware instance
   * @param component Shared pointer to hardware component
   *
   * @throws std::runtime_error if id is already registered
   */
  static void register_active(
    const std::string& id,
    std::shared_ptr<HardwareComponent> component);

  /**
   * @brief Get an active hardware component by ID
   *
   * @param id Hardware component identifier
   * @return Shared pointer to component, or nullptr if not found
   */
  static std::shared_ptr<HardwareComponent> get(const std::string& id);

  /**
   * @brief Get an active hardware component with type checking
   *
   * @tparam T Expected hardware component type
   * @param id Hardware component identifier
   * @return Shared pointer to component of type T, or nullptr if not found or wrong type
   */
  template<typename T>
  static std::shared_ptr<T> get_as(const std::string& id) {
    auto component = get(id);
    return std::dynamic_pointer_cast<T>(component);
  }

  /**
   * @brief Get all active hardware components
   *
   * @return Map of id -> component for all registered hardware
   */
  static std::map<std::string, std::shared_ptr<HardwareComponent>> get_all();

  /**
   * @brief Get all active hardware IDs
   *
   * @return Vector of registered hardware IDs
   */
  static std::vector<std::string> get_ids();

  /**
   * @brief Check if a hardware ID is registered
   *
   * @param id Hardware component identifier
   * @return true if hardware is registered, false otherwise
   */
  static bool is_registered(const std::string& id);

  /**
   * @brief Clear all active hardware components
   *
   * @note Should be called during application shutdown or when
   *       reconfiguring hardware completely.
   */
  static void clear();

  /**
   * @brief Get the number of active hardware components
   *
   * @return Count of registered hardware
   */
  static size_t count();

private:
  /**
   * @brief Get the singleton registry map
   *
   * @return Reference to the static registry map
   *
   * @note Using a function-local static ensures proper initialization order
   */
  static std::map<std::string, std::shared_ptr<HardwareComponent>>& get_registry();
};

}  // namespace trossen::hw

#endif  // TROSSEN_SDK__HW__ACTIVE_HARDWARE_REGISTRY_HPP_
