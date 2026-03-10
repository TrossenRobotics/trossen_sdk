/**
 * @file test_producer_registry.cpp
 * @brief Unit tests for ProducerRegistry (polled producers)
 *
 * Tests registration, creation, duplicate detection, error paths,
 * and enumeration of registered producer types.
 */

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"

using trossen::runtime::ProducerRegistry;

// Minimal concrete PolledProducer for testing
class MockPolledProducerForRegistry : public trossen::hw::PolledProducer {
public:
  MockPolledProducerForRegistry() = default;
  ~MockPolledProducerForRegistry() override = default;

  void poll(
    const std::function<void(std::shared_ptr<trossen::data::RecordBase>)>&) override {}

  std::shared_ptr<ProducerMetadata> metadata() const override {
    return std::make_shared<ProducerMetadata>();
  }
};

// ============================================================================
// PR-01: Unknown type is not registered
// ============================================================================

TEST(ProducerRegistryTest, IsNotRegistered_UnknownType) {
  EXPECT_FALSE(ProducerRegistry::is_registered("nonexistent_polled_type_xyz"));
}

// ============================================================================
// PR-02: Create throws for unknown type
// ============================================================================

TEST(ProducerRegistryTest, Create_ThrowsForUnknownType) {
  EXPECT_THROW(
    ProducerRegistry::create("nonexistent_polled_type_xyz", nullptr, {}),
    std::runtime_error);
}

// ============================================================================
// PR-03: Register and create mock producer
// ============================================================================

// Fixture that registers a mock factory once. The try/catch guards against
// duplicate registration when GTest re-runs the suite (static registry persists).
class ProducerRegistryRegistrationTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    try {
      ProducerRegistry::register_producer(
        "mock_polled_for_test",
        [](std::shared_ptr<trossen::hw::HardwareComponent>,
           const nlohmann::json&)
          -> std::shared_ptr<trossen::hw::PolledProducer>
        {
          return std::make_shared<MockPolledProducerForRegistry>();
        });
    } catch (const std::runtime_error&) {
      // Already registered from previous test run
    }
  }
};

TEST_F(ProducerRegistryRegistrationTest, IsRegisteredAfterRegister) {
  EXPECT_TRUE(ProducerRegistry::is_registered("mock_polled_for_test"));
}

TEST_F(ProducerRegistryRegistrationTest, CreateReturnsNonNull) {
  auto producer = ProducerRegistry::create("mock_polled_for_test", nullptr, {});
  ASSERT_NE(producer, nullptr);
}

// ============================================================================
// PR-04: Duplicate registration throws
// ============================================================================

TEST_F(ProducerRegistryRegistrationTest, DuplicateRegistration_Throws) {
  EXPECT_THROW(
    ProducerRegistry::register_producer(
      "mock_polled_for_test",
      [](std::shared_ptr<trossen::hw::HardwareComponent>,
         const nlohmann::json&)
        -> std::shared_ptr<trossen::hw::PolledProducer>
      {
        return std::make_shared<MockPolledProducerForRegistry>();
      }),
    std::runtime_error);
}

// ============================================================================
// PR-05: Registered type appears in list
// ============================================================================

TEST_F(ProducerRegistryRegistrationTest, GetRegisteredTypes_ContainsMock) {
  auto types = ProducerRegistry::get_registered_types();
  bool found = false;
  for (const auto& t : types) {
    if (t == "mock_polled_for_test") {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

// ============================================================================
// Edge cases
// ============================================================================

TEST(ProducerRegistryTest, CreateWithEmptyType_Throws) {
  EXPECT_THROW(
    ProducerRegistry::create("", nullptr, {}),
    std::runtime_error);
}

TEST(ProducerRegistryTest, IsNotRegistered_EmptyString) {
  EXPECT_FALSE(ProducerRegistry::is_registered(""));
}

// Test that GetRegisteredTypes is callable without error
TEST(ProducerRegistryTest, GetRegisteredTypes_Callable) {
  std::vector<std::string> types;
  EXPECT_NO_THROW(types = ProducerRegistry::get_registered_types());
}

// Test with null factory return
class ProducerRegistryNullFactoryTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    try {
      ProducerRegistry::register_producer(
        "null_factory_polled_for_test",
        [](std::shared_ptr<trossen::hw::HardwareComponent>,
           const nlohmann::json&)
          -> std::shared_ptr<trossen::hw::PolledProducer>
        {
          return nullptr;
        });
    } catch (const std::runtime_error&) {
      // Already registered
    }
  }
};

TEST_F(ProducerRegistryNullFactoryTest, CreateThrowsWhenFactoryReturnsNull) {
  EXPECT_THROW(
    ProducerRegistry::create("null_factory_polled_for_test", nullptr, {}),
    std::runtime_error);
}
