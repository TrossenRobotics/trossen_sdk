/**
 * @file transport_registry.hpp
 * @brief Static factory registry mapping config transport names to
 *        PolicyTransport factories.
 *
 * Mirrors ObserverRegistry's shape (function-local static variables, lookup
 * under a mutex, factory invoked outside the lock, errors annotated with the
 * name).
 * Built-in transports self-register from their own translation units; user
 * plugins call register_factory() before configuring a PolicyClient.
 */

#ifndef TROSSEN_SDK__HW__POLICY__TRANSPORT_REGISTRY_HPP_
#define TROSSEN_SDK__HW__POLICY__TRANSPORT_REGISTRY_HPP_

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace trossen::hw::policy {

class PolicyTransport;

/**
 * @brief Process-wide name -> factory map for policy transports.
 *
 * The PolicyClient resolves its config's ``"transport"`` string (default
 * ``"openpi_ws"``) here and never names a concrete transport class.
 */
class TransportRegistry {
public:
  /**
   * @brief Factory signature.
   *
   * @param id               Owning PolicyClient's logical id (for log prefixes).
   * @param server_url       Endpoint string; SCHEME VALIDATION IS THE FACTORY'S JOB
   *                         (``ws://...`` for openpi_ws, ``host:port`` for grpc, ...).
   * @param transport_config Opaque per-transport JSON object, passed verbatim
   *                         from the config's ``"transport_config"`` field
   *                         (empty object when absent).
   * @return Non-null, not-yet-connected transport. Throwing or returning
   *         nullptr is treated as a configuration error.
   */
  using FactoryFunc = std::function<std::unique_ptr<PolicyTransport>(
        const std::string & id,
        const std::string & server_url,
        const nlohmann::json & transport_config)>;

  /**
   * @brief Register a transport factory under a unique name.
   *
   * Thread-safe. Intended for static-init self-registration of built-ins and
   * for user plugins at startup.
   *
   * @throws std::runtime_error if @p name is empty, @p factory is null, or
   *         @p name is already registered.
   */
  static void register_factory(const std::string & name, FactoryFunc factory);

  /**
   * @brief Build a transport by registered name.
   *
   * The factory runs outside the registry lock (it may call back in).
   *
   * @throws std::runtime_error if @p name is unknown, the factory returns
   *         nullptr, or the factory throws (re-thrown with @p name annotated).
   */
  static std::unique_ptr<PolicyTransport> create(
    const std::string & name,
    const std::string & id,
    const std::string & server_url,
    const nlohmann::json & transport_config);

  /// True if @p name has been registered. Thread-safe.
  static bool is_registered(const std::string & name);

  /// All registered names (no ordering guarantee). Thread-safe.
  static std::vector<std::string> get_registered_names();

private:
  // Function-local static variables: immune to the static-init-order fiasco
  // that class-level static variables would expose to self-registering
  // translation units.
  static std::map<std::string, FactoryFunc> & get_registry();
  static std::mutex & get_mutex();
};

}  // namespace trossen::hw::policy

#endif  // TROSSEN_SDK__HW__POLICY__TRANSPORT_REGISTRY_HPP_
