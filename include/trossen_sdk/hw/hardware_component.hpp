/**
 * @file hardware_component.hpp
 * @brief Abstract base interface for hardware components
 */

#ifndef TROSSEN_SDK__HW__HARDWARE_COMPONENT_HPP_
#define TROSSEN_SDK__HW__HARDWARE_COMPONENT_HPP_

#include <memory>
#include <string>

#include "nlohmann/json.hpp"

namespace trossen::hw {

/**
 * @brief Abstract base class for all hardware components
 *
 * Hardware components represent individually controlled or monitored pieces of hardware (robot
 * arms, cameras, sensors, etc.). Each component can be configured via JSON and queried for its
 * type.
 */
class HardwareComponent {
public:
  /**
   * @brief Constructor
   *
   * @param identifier Optional component identifier
   */
  explicit HardwareComponent(const std::string & identifier) : identifier_(identifier) {}
  virtual ~HardwareComponent() = default;

  /**
   * @brief Configure the hardware component from JSON
   *
   * @param config JSON configuration object
   * @throws std::runtime_error if configuration fails
   *
   * @note Configuration structure is component-specific. Implementations should validate required
   *       fields and apply settings to underlying hardware drivers.
   */
  virtual void configure(const nlohmann::json& config) = 0;

  /**
   * @brief Get the identifier string for this hardware component
   *
   * @return identifier
   */
  const std::string & get_identifier() const { return identifier_; }

  /**
   * @brief Get the type string for this hardware component
   *
   * @return Type identifier (e.g., "trossen_arm", "opencv_camera")
   */
  virtual std::string get_type() const = 0;

  /**
   * @brief Get human-readable component information (optional)
   *
   * @return JSON object with component details (model, serial, etc.)
   */
  virtual nlohmann::json get_info() const { return nlohmann::json::object(); }

protected:
  /// @brief Component identifier
  std::string identifier_;
};

}  // namespace trossen::hw

#endif  // TROSSEN_SDK__HW__HARDWARE_COMPONENT_HPP_
