/**
 * @file backend_registry.hpp
 * @brief Factory registry for backend types.
 *
 * Provides a static registry that maps backend type strings to factory functions,
 * enabling extensible backend creation without hardcoded type-checking.
 */

#ifndef TROSSEN_SDK__IO__BACKEND_REGISTRY_HPP
#define TROSSEN_SDK__IO__BACKEND_REGISTRY_HPP

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/io/backend.hpp"
#include "trossen_sdk/types.hpp"

namespace trossen::io {

/**
 * @brief Static registry for backend factory functions.
 *
 * Backend implementations register themselves at static initialization time.
 */
class BackendRegistry {
public:
  /// @brief Factory function signature for creating backend instances
  using FactoryFunc = std::function<std::shared_ptr<Backend>(
    Backend::Config&,
    const ProducerMetadataList&)>;

  /**
   * @brief Register a backend factory function
   *
   * @param type Backend type string (e.g., "mcap", "lerobot")
   * @param factory Factory function that creates backend instances
   *
   * @throws std::runtime_error if type is already registered
   *
   * @note This should be called during static initialization, typically using a static registrar
   *       object in the backend implementation file.
   */
  static void register_backend(const std::string& type, FactoryFunc factory);

  /**
   * @brief Create a backend instance by type
   *
   * @param type Backend type string
   * @param config Backend configuration (must be compatible with registered type)
   * @param producer_metadatas Optional vector of producer metadata (used by some backends)
   *
   * @return Shared pointer to created backend instance
   *
   * @throws std::runtime_error if type is not registered
   *
   * @note The config reference is downcast to the appropriate concrete config type by the
   *       registered factory function.
   */
  static std::shared_ptr<Backend> create(
    const std::string& type,
    Backend::Config& config,
    const ProducerMetadataList& producer_metadatas = {});

  /**
   * @brief Check if a backend type is registered
   *
   * @param type Backend type string
   * @return true if the type is registered, false otherwise
   */
  static bool is_registered(const std::string& type);

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
 * @brief Macro to register a backend type with the BackendRegistry
 *
 * This macro creates a static registrar object that registers the backend factory function during
 * static initialization. Use this in your backend implementation (.cpp) file.
 *
 * @param ClassName The backend class name (e.g., FooBackend)
 * @param TypeString The type string for this backend (e.g., "foo")
 *
 * Example usage:
 * @code
 * namespace trossen::io::backends { REGISTER_BACKEND(FooBackend, "foo")
 * }
 * @endcode
 */
#define REGISTER_BACKEND(ClassName, TypeString)                                          \
  namespace {                                                                            \
  struct ClassName##Registrar {                                                          \
    ClassName##Registrar() {                                                             \
      ::trossen::io::BackendRegistry::register_backend(                                  \
        TypeString,                                                                      \
        [](::trossen::io::Backend::Config& cfg,                                          \
           const ::trossen::ProducerMetadataList& metadata)                              \
             -> std::shared_ptr<::trossen::io::Backend> {                                \
          return std::make_shared<ClassName>(static_cast<ClassName::Config&>(cfg), metadata); \
        });                                                                              \
    }                                                                                    \
  };                                                                                     \
  static ClassName##Registrar ClassName##_registrar_instance;                           \
  }  /* anonymous namespace */

}  // namespace trossen::io

#endif  // TROSSEN_SDK__IO__BACKEND_REGISTRY_HPP
