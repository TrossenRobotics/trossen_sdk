/**
 * @file test_observer_registry.cpp
 * @brief Tests for ObserverRegistry and ObserverConfig JSON parsing.
 */

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/sdk_config.hpp"
#include "trossen_sdk/configuration/types/observers/observer_config.hpp"
#include "trossen_sdk/observer/observer_base.hpp"
#include "trossen_sdk/observer/observer_registry.hpp"

using nlohmann::json;
using trossen::configuration::ObserverConfig;
using trossen::configuration::ObserverSubscriptionConfig;
using trossen::configuration::SdkConfig;
using trossen::observer::ObserverBase;
using trossen::observer::ObserverRegistry;

namespace {

// Minimal concrete observer that reads its subscriptions from the JSON config.
class DummyObserver : public ObserverBase {
public:
  explicit DummyObserver(const json& cfg)
    : ObserverBase(cfg.value("id", std::string("dummy"))) {
    if (!cfg.contains("subscriptions") || !cfg.at("subscriptions").is_array()) {
      throw std::runtime_error("DummyObserver: missing 'subscriptions'");
    }
    for (const auto& sub : cfg.at("subscriptions")) {
      add_subscription(
        sub.at("record_id").get<std::string>(),
        sub.at("throttle_hz").get<double>(),
        [](const std::shared_ptr<trossen::data::RecordBase>&) {});
    }
  }
};

}  // namespace

// ----------------------------------------------------------------------------
// ObserverSubscriptionConfig
// ----------------------------------------------------------------------------

TEST(ObserverSubscriptionConfigTest, Parses_Valid) {
  auto j = json::parse(R"({"record_id": "arm", "throttle_hz": 30.0})");
  auto c = ObserverSubscriptionConfig::from_json(j);
  EXPECT_EQ(c.record_id, "arm");
  EXPECT_DOUBLE_EQ(c.throttle_hz, 30.0);
}

TEST(ObserverSubscriptionConfigTest, Rejects_MissingOrEmptyRecordId) {
  EXPECT_THROW(ObserverSubscriptionConfig::from_json(json::parse(R"({"throttle_hz": 1.0})")),
               std::runtime_error);
  EXPECT_THROW(ObserverSubscriptionConfig::from_json(
                 json::parse(R"({"record_id": "", "throttle_hz": 1.0})")),
               std::runtime_error);
}

TEST(ObserverSubscriptionConfigTest, Rejects_NonPositiveThrottle) {
  EXPECT_THROW(ObserverSubscriptionConfig::from_json(
                 json::parse(R"({"record_id": "x", "throttle_hz": 0})")),
               std::runtime_error);
  EXPECT_THROW(ObserverSubscriptionConfig::from_json(
                 json::parse(R"({"record_id": "x", "throttle_hz": -5})")),
               std::runtime_error);
}

// ----------------------------------------------------------------------------
// ObserverConfig
// ----------------------------------------------------------------------------

TEST(ObserverConfigTest, Parses_Valid_FullForm) {
  auto j = json::parse(R"({
    "type": "rerun",
    "id":   "live",
    "subscriptions": [
      {"record_id": "arm",  "throttle_hz": 30.0},
      {"record_id": "cam0", "throttle_hz": 15.0}
    ],
    "address": "127.0.0.1:9876"
  })");
  auto c = ObserverConfig::from_json(j);
  EXPECT_EQ(c.type, "rerun");
  EXPECT_EQ(c.id, "live");
  ASSERT_EQ(c.subscriptions.size(), 2u);
  EXPECT_EQ(c.subscriptions[0].record_id, "arm");
  EXPECT_DOUBLE_EQ(c.subscriptions[1].throttle_hz, 15.0);
  // raw_json must be preserved so the factory can read type-specific fields.
  EXPECT_EQ(c.raw_json.at("address").get<std::string>(), "127.0.0.1:9876");
}

TEST(ObserverConfigTest, DefaultsId_ToType_WhenOmitted) {
  auto j = json::parse(R"({
    "type": "rerun",
    "subscriptions": [{"record_id": "arm", "throttle_hz": 30.0}]
  })");
  auto c = ObserverConfig::from_json(j);
  EXPECT_EQ(c.id, "rerun");
}

TEST(ObserverConfigTest, Rejects_MissingType) {
  auto j = json::parse(R"({
    "subscriptions": [{"record_id": "arm", "throttle_hz": 30.0}]
  })");
  EXPECT_THROW(ObserverConfig::from_json(j), std::runtime_error);
}

TEST(ObserverConfigTest, Rejects_MissingOrEmptySubscriptions) {
  auto missing = json::parse(R"({"type": "rerun"})");
  EXPECT_THROW(ObserverConfig::from_json(missing), std::runtime_error);

  auto empty = json::parse(R"({"type": "rerun", "subscriptions": []})");
  EXPECT_THROW(ObserverConfig::from_json(empty), std::runtime_error);

  auto wrong_type =
    json::parse(R"({"type": "rerun", "subscriptions": "not_an_array"})");
  EXPECT_THROW(ObserverConfig::from_json(wrong_type), std::runtime_error);
}

// ----------------------------------------------------------------------------
// SdkConfig integration
// ----------------------------------------------------------------------------

TEST(SdkConfigObserversTest, Parses_TopLevelObservers) {
  auto j = json::parse(R"({
    "robot_name": "test_bot",
    "observers": [
      {
        "type": "rerun",
        "subscriptions": [{"record_id": "arm", "throttle_hz": 30.0}]
      },
      {
        "type": "policy_client",
        "id":   "policy",
        "subscriptions": [{"record_id": "cam", "throttle_hz": 15.0}]
      }
    ]
  })");
  auto cfg = SdkConfig::from_json(j);
  ASSERT_EQ(cfg.observers.size(), 2u);
  EXPECT_EQ(cfg.observers[0].type, "rerun");
  EXPECT_EQ(cfg.observers[0].id,   "rerun");
  EXPECT_EQ(cfg.observers[1].id,   "policy");
}

TEST(SdkConfigObserversTest, Allows_NoObserversKey) {
  auto j = json::parse(R"({"robot_name": "test_bot"})");
  auto cfg = SdkConfig::from_json(j);
  EXPECT_TRUE(cfg.observers.empty());
}

TEST(SdkConfigObserversTest, Rejects_ObserversNotArray) {
  auto j = json::parse(R"({
    "robot_name": "test_bot",
    "observers": {"type": "rerun"}
  })");
  EXPECT_THROW(SdkConfig::from_json(j), std::runtime_error);
}

TEST(SdkConfigObserversTest, Rejects_DuplicateObserverIds) {
  // Two observers with the same explicit id must be rejected so logs and observer_stats()
  // entries can distinguish them. Defaulted-from-type collisions (two {"type":"rerun"}
  // entries with no explicit id) are also caught by the same check.
  auto explicit_dup = json::parse(R"({
    "observers": [
      { "type": "rerun", "id": "live",
        "subscriptions": [{"record_id": "arm", "throttle_hz": 30.0}] },
      { "type": "rerun", "id": "live",
        "subscriptions": [{"record_id": "cam", "throttle_hz": 15.0}] }
    ]
  })");
  EXPECT_THROW(SdkConfig::from_json(explicit_dup), std::runtime_error);

  auto defaulted_dup = json::parse(R"({
    "observers": [
      { "type": "rerun",
        "subscriptions": [{"record_id": "arm", "throttle_hz": 30.0}] },
      { "type": "rerun",
        "subscriptions": [{"record_id": "cam", "throttle_hz": 15.0}] }
    ]
  })");
  EXPECT_THROW(SdkConfig::from_json(defaulted_dup), std::runtime_error);
}

// ----------------------------------------------------------------------------
// ObserverRegistry
// ----------------------------------------------------------------------------

TEST(ObserverRegistryTest, RegisterAndCreate_RoundTrip) {
  const std::string type = "dummy_round_trip";
  ASSERT_FALSE(ObserverRegistry::is_registered(type));

  ObserverRegistry::register_observer(type, [](const json& cfg) {
    return std::make_shared<DummyObserver>(cfg);
  });
  EXPECT_TRUE(ObserverRegistry::is_registered(type));

  auto cfg = json::parse(R"({
    "type": "dummy_round_trip",
    "subscriptions": [{"record_id": "arm", "throttle_hz": 30.0}]
  })");
  auto obs = ObserverRegistry::create(type, cfg);
  ASSERT_NE(obs, nullptr);
  EXPECT_EQ(obs->subscription_count(), 1u);
}

TEST(ObserverRegistryTest, Register_DuplicateType_Throws) {
  const std::string type = "dummy_duplicate";
  ObserverRegistry::register_observer(type, [](const json& cfg) {
    return std::make_shared<DummyObserver>(cfg);
  });
  EXPECT_THROW(ObserverRegistry::register_observer(type, [](const json& cfg) {
    return std::make_shared<DummyObserver>(cfg);
  }), std::runtime_error);
}

TEST(ObserverRegistryTest, Register_NullFactory_Throws) {
  EXPECT_THROW(
    ObserverRegistry::register_observer("dummy_null", ObserverRegistry::FactoryFunc{}),
    std::runtime_error);
}

TEST(ObserverRegistryTest, Create_UnknownType_Throws) {
  EXPECT_THROW(ObserverRegistry::create("never_registered_type", json::object()),
               std::runtime_error);
}

TEST(ObserverRegistryTest, Create_NullReturningFactory_Throws) {
  const std::string type = "dummy_returns_null";
  ObserverRegistry::register_observer(type, [](const json&) {
    return std::shared_ptr<ObserverBase>{};
  });
  EXPECT_THROW(ObserverRegistry::create(type, json::object()), std::runtime_error);
}

TEST(ObserverRegistryTest, GetRegisteredTypes_IncludesAdded) {
  const std::string type = "dummy_list";
  ObserverRegistry::register_observer(type, [](const json& cfg) {
    return std::make_shared<DummyObserver>(cfg);
  });
  const auto types = ObserverRegistry::get_registered_types();
  bool found = false;
  for (const auto& t : types) {
    if (t == type) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}
