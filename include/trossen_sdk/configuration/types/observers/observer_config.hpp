/**
 * @file observer_config.hpp
 * @brief Configuration for a single non-durable Observer.
 *
 * Common fields (``type``, ``id``, ``subscriptions``) are parsed up front; type-specific
 * fields stay in ``raw_json`` for the ``ObserverRegistry`` factory.
 *
 * JSON format:
 * @code
 * {
 *   "type": "rerun",
 *   "id":   "live_viewer",
 *   "subscriptions": [
 *     { "record_id": "follower_left",  "throttle_hz": 30.0 },
 *     { "record_id": "cam_high/color", "throttle_hz": 15.0 }
 *   ],
 *   "address": "127.0.0.1:9876"
 * }
 * @endcode
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__OBSERVERS__OBSERVER_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__OBSERVERS__OBSERVER_CONFIG_HPP_

#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "nlohmann/json.hpp"

namespace trossen::configuration {

/**
 * @brief Per-stream subscription for one Observer.
 */
struct ObserverSubscriptionConfig {
  /// Exact match against ``RecordBase::id``.
  std::string record_id;

  /// Maximum handler dispatch rate (Hz); must lie within [1e-3, 1e4].
  double throttle_hz{0.0};

  static ObserverSubscriptionConfig from_json(const nlohmann::json& j) {
    ObserverSubscriptionConfig c;
    if (!j.is_object()) {
      throw std::runtime_error(
        "ObserverSubscriptionConfig: entry must be a JSON object");
    }
    if (j.contains("record_id")) {
      if (!j.at("record_id").is_string()) {
        throw std::runtime_error(
          "ObserverSubscriptionConfig: 'record_id' must be a string");
      }
      j.at("record_id").get_to(c.record_id);
    }
    if (j.contains("throttle_hz")) {
      if (!j.at("throttle_hz").is_number()) {
        throw std::runtime_error(
          "ObserverSubscriptionConfig: 'throttle_hz' must be a number for record_id '" +
          c.record_id + "'");
      }
      j.at("throttle_hz").get_to(c.throttle_hz);
    }
    if (c.record_id.empty()) {
      throw std::runtime_error(
        "ObserverSubscriptionConfig: 'record_id' is required and must be non-empty");
    }
    // Mirror ObserverBase::add_subscription range so configs fail fast at parse time
    // rather than later at construction. The bounded-range check below also rejects
    // NaN (any comparison with NaN is false, so the negation throws).
    constexpr double kMinThrottleHz = 1e-3;
    constexpr double kMaxThrottleHz = 1e4;
    if (!(c.throttle_hz >= kMinThrottleHz && c.throttle_hz <= kMaxThrottleHz)) {
      throw std::runtime_error(
        "ObserverSubscriptionConfig: 'throttle_hz' must be within [1e-3, 1e4] for "
        "record_id '" + c.record_id + "'");
    }
    return c;
  }
};

/**
 * @brief Configuration for one Observer instance.
 *
 * Common fields are parsed up front; the full JSON object is also retained in ``raw_json``
 * so the registry factory can read any type-specific fields without bouncing through
 * another schema.
 */
struct ObserverConfig {
  /// Registry key (e.g. "rerun", "policy_client"). Required.
  std::string type;

  /// Logging identifier. Defaults to ``type`` if omitted.
  std::string id;

  /// Per-stream subscriptions. At least one required.
  std::vector<ObserverSubscriptionConfig> subscriptions;

  /// When false, this observer is parsed and validated but not instantiated. Defaults
  /// to true.
  bool enabled{true};

  /// Original JSON object; preserved so the factory can read type-specific fields.
  nlohmann::json raw_json;

  static ObserverConfig from_json(const nlohmann::json& j) {
    ObserverConfig c;
    if (!j.is_object()) {
      throw std::runtime_error(
        "ObserverConfig: entry must be a JSON object");
    }
    c.raw_json = j;

    if (j.contains("type")) {
      if (!j.at("type").is_string()) {
        throw std::runtime_error(
          "ObserverConfig: 'type' must be a string");
      }
      j.at("type").get_to(c.type);
    }
    if (c.type.empty()) {
      throw std::runtime_error(
        "ObserverConfig: 'type' is required and must be non-empty");
    }

    if (j.contains("id")) {
      if (!j.at("id").is_string()) {
        throw std::runtime_error(
          "ObserverConfig: 'id' must be a string for observer of type '" + c.type + "'");
      }
      j.at("id").get_to(c.id);
    }
    if (c.id.empty()) {
      c.id = c.type;
    }
    if (j.contains("enabled")) {
      if (!j.at("enabled").is_boolean()) {
        throw std::runtime_error(
          "ObserverConfig: 'enabled' must be a boolean for observer '" + c.id + "'");
      }
      j.at("enabled").get_to(c.enabled);
    }

    if (!j.contains("subscriptions") || !j.at("subscriptions").is_array()) {
      throw std::runtime_error(
        "ObserverConfig: 'subscriptions' (array) is required for observer '" + c.id + "'");
    }
    for (const auto& sub_j : j.at("subscriptions")) {
      try {
        c.subscriptions.push_back(ObserverSubscriptionConfig::from_json(sub_j));
      } catch (const std::exception& e) {
        throw std::runtime_error(
          "ObserverConfig: failed to parse subscription for observer '" + c.id + "': " +
          e.what());
      }
    }
    if (c.subscriptions.empty()) {
      throw std::runtime_error(
        "ObserverConfig: 'subscriptions' array must be non-empty for observer '" +
        c.id + "'");
    }
    // Reject duplicate record_ids inside a single observer's subscriptions; mirrors
    // ObserverBase::add_subscription which throws on duplicates at construction.
    std::unordered_set<std::string> seen_record_ids;
    for (const auto& sub : c.subscriptions) {
      if (!seen_record_ids.insert(sub.record_id).second) {
        throw std::runtime_error(
          "ObserverConfig: duplicate record_id '" + sub.record_id +
          "' in subscriptions for observer '" + c.id + "'");
      }
    }

    return c;
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__OBSERVERS__OBSERVER_CONFIG_HPP_
