/**
 * @file observer_registry.hpp
 * @brief Static factory registry for Observer types.
 */

#ifndef TROSSEN_SDK__OBSERVER__OBSERVER_REGISTRY_HPP
#define TROSSEN_SDK__OBSERVER__OBSERVER_REGISTRY_HPP

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "trossen_sdk/observer/observer_base.hpp"

namespace trossen::observer {

/**
 * @brief Static factory registry for Observer types.
 *
 * Each observer type advertises one factory function that turns a JSON config object into
 * a concrete ``ObserverBase``-derived instance with its subscriptions already wired up.
 * Lookup is by exact type-string match.
 */
class ObserverRegistry {
public:
  /**
   * @brief Factory function signature.
   *
   * @param config JSON object describing the observer (the same object that lives in the
   *               top-level ``"observers"`` array). The factory is responsible for
   *               consuming whatever transport-specific fields it needs and for calling
   *               ``add_subscription()`` on the returned observer per the JSON's
   *               ``"subscriptions"`` array.
   * @return Non-null shared_ptr to a constructed observer. Throwing or returning nullptr is
   *         treated as a configuration error.
   */
  using FactoryFunc =
    std::function<std::shared_ptr<ObserverBase>(const nlohmann::json& config)>;

  /**
   * @brief Register an observer factory.
   *
   * Thread-safe: serialized via an internal mutex. Intended for static-init-time
   * use via ``REGISTER_OBSERVER``, but safe to call at any time.
   *
   * @param type Non-empty type string (e.g. "rerun"). Must be unique within the process.
   * @param factory Non-null factory callable.
   * @throws std::runtime_error if ``type`` is empty, ``factory`` is null, or ``type`` is
   *         already registered.
   */
  static void register_observer(const std::string& type, FactoryFunc factory);

  /**
   * @brief Create an observer by type.
   *
   * Thread-safe: the registry lookup is serialized via an internal mutex. The factory is
   * invoked without the lock held, so factories may freely call back into the registry.
   *
   * @param type Type string previously passed to ``register_observer``.
   * @param config JSON object handed straight to the factory.
   * @return Constructed observer.
   * @throws std::runtime_error if the type is not registered, the factory yields nullptr,
   *         or the factory itself throws (exceptions are re-thrown with the type string
   *         annotated).
   */
  static std::shared_ptr<ObserverBase> create(
    const std::string& type,
    const nlohmann::json& config);

  /// True if @p type has been registered. Thread-safe.
  static bool is_registered(const std::string& type);

  /// List of all registered type strings (no ordering guarantee). Thread-safe.
  static std::vector<std::string> get_registered_types();

private:
  static std::map<std::string, FactoryFunc>& get_registry();
  static std::mutex& get_mutex();
};

/**
 * @brief Register an Observer subclass with the registry at static-init time.
 *
 * Generates a static registrar object whose constructor calls
 * ``ObserverRegistry::register_observer`` with a factory that constructs ``ClassName``
 * from the JSON config alone.
 *
 * @param ClassName Observer subclass. Must be constructible from
 *                  ``const ::nlohmann::json&`` -- the factory calls
 *                  ``std::make_shared<ClassName>(config)``.
 * @param TypeString Unique type-string token (e.g. ``"rerun"``).
 *
 * Trailing semicolons are permitted: the macro absorbs one so
 * ``REGISTER_OBSERVER(Foo, "foo");`` compiles cleanly under ``-Wpedantic``.
 *
 * Example:
 * @code
 *   // rerun_observer.cpp
 *   namespace trossen::observer {
 *   REGISTER_OBSERVER(RerunObserver, "rerun")
 *   }
 * @endcode
 */
#define REGISTER_OBSERVER(ClassName, TypeString)                                       \
  namespace {                                                                          \
  struct ClassName##Registrar {                                                        \
    ClassName##Registrar() {                                                           \
      ::trossen::observer::ObserverRegistry::register_observer(                        \
        TypeString,                                                                    \
        [](const ::nlohmann::json& cfg)                                                \
             -> std::shared_ptr<::trossen::observer::ObserverBase> {                   \
          return std::make_shared<ClassName>(cfg);                                     \
        });                                                                            \
    }                                                                                  \
  };                                                                                   \
  static ClassName##Registrar ClassName##_registrar_instance;                          \
  }  /* anonymous namespace */                                                         \
  static_assert(true, "REGISTER_OBSERVER trailing-semicolon sink")

}  // namespace trossen::observer

#endif  // TROSSEN_SDK__OBSERVER__OBSERVER_REGISTRY_HPP
