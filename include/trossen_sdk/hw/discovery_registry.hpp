/**
 * @file discovery_registry.hpp
 * @brief Static registry for hardware-discovery functions.
 */

#ifndef TROSSEN_SDK__HW__DISCOVERY_REGISTRY_HPP_
#define TROSSEN_SDK__HW__DISCOVERY_REGISTRY_HPP_

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace trossen::hw {

/**
 * @brief Result of a hardware-discovery probe for a single device.
 *
 * Produced by each hardware component's static @c find() method. The fixed
 * fields identify the device well enough to populate a config; @c details
 * carries any type-specific metadata (resolution for cameras, firmware for
 * bases, etc.) as free-form JSON, matching the style already used by
 * @c HardwareComponent::get_info().
 */
struct DiscoveredHardware {
  /// @brief HardwareRegistry type key (e.g. "realsense_camera", "trossen_arm").
  std::string type;

  /// @brief Unique identifier usable in config (serial number, device index, ...).
  std::string identifier;

  /// @brief Human-readable model / product name, if known.
  std::string product_name;

  /// @brief Type-specific discovery metadata. See the emitting component's find() docs.
  nlohmann::json details;

  /// @brief True if the probe completed end-to-end (including any side artifact).
  bool ok = true;
};

/**
 * @brief Static registry mapping a hardware type key to its discovery function.
 *
 * Mirrors @c HardwareRegistry: components self-register at static-init time
 * via the @c REGISTER_HARDWARE_DISCOVERY macro. Types that have no enumeration
 * API simply don't register; @c find() returns @c std::nullopt for those.
 *
 * Discovery is intentionally opt-in: a component can register with
 * @c HardwareRegistry (construction) but not here (no way to enumerate), or
 * vice versa.
 */
class DiscoveryRegistry {
 public:
  /// @brief Discovery function signature: given an output directory, return
  ///        one @c DiscoveredHardware per detected device.
  using DiscoveryFunc =
    std::function<std::vector<DiscoveredHardware>(const std::filesystem::path&)>;

  /**
   * @brief Register a discovery function for a hardware type.
   *
   * @param type Hardware type key (must match @c HardwareRegistry key for this
   *             component, e.g. @c "realsense_camera").
   * @param fn   Function that enumerates devices and produces preview artifacts
   *             into @p output_dir.
   * @throws std::runtime_error if @p type is already registered.
   */
  static void register_discovery(const std::string& type, DiscoveryFunc fn);

  /**
   * @brief Run the registered discovery function for @p type.
   *
   * @param type       Hardware type key.
   * @param output_dir Forwarded to the discovery function for any side artifacts.
   * @return The discovery result, or @c std::nullopt if @p type has no
   *         registered discovery function.
   */
  static std::optional<std::vector<DiscoveredHardware>> find(
    const std::string& type,
    const std::filesystem::path& output_dir);

  /// @brief Whether a discovery function is registered for @p type.
  static bool is_registered(const std::string& type);

  /// @brief All type keys with a registered discovery function.
  static std::vector<std::string> supported_types();

 private:
  static std::map<std::string, DiscoveryFunc>& get_registry();
};

/**
 * @brief Register a static @c find() method as a discovery function.
 *
 * Mirrors @c REGISTER_HARDWARE. Place alongside the existing registration in
 * the component's @c .cpp file.
 *
 * @param ClassName  Component class with a @c static std::vector<DiscoveredHardware>
 *                   find(const std::filesystem::path&).
 * @param TypeString Hardware type key, matching the @c REGISTER_HARDWARE key.
 *
 * Example:
 * @code
 * REGISTER_HARDWARE(RealsenseCameraComponent, "realsense_camera")
 * REGISTER_HARDWARE_DISCOVERY(RealsenseCameraComponent, "realsense_camera")
 * @endcode
 */
#define REGISTER_HARDWARE_DISCOVERY(ClassName, TypeString)              \
  namespace {                                                           \
  struct ClassName##DiscoveryRegistrar {                                \
    ClassName##DiscoveryRegistrar() {                                   \
      ::trossen::hw::DiscoveryRegistry::register_discovery(             \
        TypeString, &ClassName::find);                                  \
    }                                                                   \
  };                                                                    \
  static ClassName##DiscoveryRegistrar                                  \
    ClassName##_discovery_registrar_instance;                           \
  }  /* anonymous namespace */

}  // namespace trossen::hw

#endif  // TROSSEN_SDK__HW__DISCOVERY_REGISTRY_HPP_
