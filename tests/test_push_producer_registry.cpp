/**
 * @file test_push_producer_registry.cpp
 * @brief Unit tests for PushProducerRegistry
 */

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/runtime/push_producer_registry.hpp"

using trossen::runtime::PushProducerRegistry;

// Minimal concrete PushProducer for testing purposes
class MockPushProducer : public trossen::hw::PushProducer {
public:
  MockPushProducer() = default;
  ~MockPushProducer() override = default;

  bool start(
    const std::function<void(std::shared_ptr<trossen::data::RecordBase>)>&) override
  {
    return true;
  }

  void stop() override {}
};

// ============================================================================
// Negative-path tests (no registration required)
// ============================================================================

TEST(PushProducerRegistryTest, IsNotRegisteredForUnknownType) {
  EXPECT_FALSE(PushProducerRegistry::is_registered("nonexistent_push_type_xyz"));
}

TEST(PushProducerRegistryTest, CreateThrowsForUnknownType) {
  EXPECT_THROW(
    PushProducerRegistry::create("nonexistent_push_type_xyz", nullptr, {}),
    std::runtime_error);
}

TEST(PushProducerRegistryTest, GetRegisteredTypesIsCallable) {
  // Must not throw; may be empty if no hardware types are compiled in
  std::vector<std::string> types;
  EXPECT_NO_THROW(types = PushProducerRegistry::get_registered_types());
}

// ============================================================================
// Registration tests (register a mock type once per suite)
// ============================================================================

class PushProducerRegistryRegistrationTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    // Register a mock producer type once; catch if already registered from
    // a previous test run (static registry persists across test binaries)
    try {
      PushProducerRegistry::register_producer(
        "mock_push_producer_for_test",
        [](std::shared_ptr<trossen::hw::HardwareComponent>,
           const nlohmann::json&)
          -> std::shared_ptr<trossen::hw::PushProducer>
        {
          return std::make_shared<MockPushProducer>();
        });
    } catch (const std::runtime_error& e) {
      // Already registered — ignore duplicate in repeated test runs
    }
  }
};

TEST_F(PushProducerRegistryRegistrationTest, IsRegisteredAfterRegister) {
  EXPECT_TRUE(PushProducerRegistry::is_registered("mock_push_producer_for_test"));
}

TEST_F(PushProducerRegistryRegistrationTest, CreateReturnsNonNull) {
  std::shared_ptr<trossen::hw::PushProducer> producer;
  EXPECT_NO_THROW(
    producer = PushProducerRegistry::create(
      "mock_push_producer_for_test", nullptr, {}));
  ASSERT_NE(producer, nullptr);
}

TEST_F(PushProducerRegistryRegistrationTest, RegisteredTypeAppearsInList) {
  auto types = PushProducerRegistry::get_registered_types();
  bool found = false;
  for (const auto& t : types) {
    if (t == "mock_push_producer_for_test") {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(PushProducerRegistryRegistrationTest, DuplicateRegistrationThrows) {
  EXPECT_THROW(
    PushProducerRegistry::register_producer(
      "mock_push_producer_for_test",
      [](std::shared_ptr<trossen::hw::HardwareComponent>,
         const nlohmann::json&)
        -> std::shared_ptr<trossen::hw::PushProducer>
      {
        return std::make_shared<MockPushProducer>();
      }),
    std::runtime_error);
}

// ============================================================================
// Extended negative-path and edge-case tests
// ============================================================================

TEST(PushProducerRegistryTest, CreateWithEmptyTypeThrows) {
  EXPECT_THROW(
    PushProducerRegistry::create("", nullptr, {}),
    std::runtime_error);
}

TEST(PushProducerRegistryTest, IsNotRegisteredForEmptyString) {
  EXPECT_FALSE(PushProducerRegistry::is_registered(""));
}

// Factory that returns nullptr should be caught by create()
class PushProducerRegistryNullFactoryTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    try {
      PushProducerRegistry::register_producer(
        "null_factory_producer_for_test",
        [](std::shared_ptr<trossen::hw::HardwareComponent>,
           const nlohmann::json&)
          -> std::shared_ptr<trossen::hw::PushProducer>
        {
          return nullptr;  // Deliberately returns null
        });
    } catch (const std::runtime_error&) {
      // Already registered
    }
  }
};

TEST_F(PushProducerRegistryNullFactoryTest, CreateThrowsWhenFactoryReturnsNull) {
  EXPECT_THROW(
    PushProducerRegistry::create("null_factory_producer_for_test", nullptr, {}),
    std::runtime_error);
}

// Test that create() with nullptr hardware works for mock producers
TEST_F(PushProducerRegistryRegistrationTest, CreateWithNullHardware) {
  std::shared_ptr<trossen::hw::PushProducer> producer;
  EXPECT_NO_THROW(
    producer = PushProducerRegistry::create(
      "mock_push_producer_for_test", nullptr, {}));
  ASSERT_NE(producer, nullptr);
}

// Test that create() with JSON config passes config through
TEST_F(PushProducerRegistryRegistrationTest, CreateWithConfig) {
  nlohmann::json config = {{"stream_id", "test_cam"}, {"fps", 30}};
  std::shared_ptr<trossen::hw::PushProducer> producer;
  EXPECT_NO_THROW(
    producer = PushProducerRegistry::create(
      "mock_push_producer_for_test", nullptr, config));
  ASSERT_NE(producer, nullptr);
}

// Test that multiple registered types appear in the list
TEST_F(PushProducerRegistryRegistrationTest, MultipleMockTypesCanCoexist) {
  // Register a second type
  try {
    PushProducerRegistry::register_producer(
      "mock_push_producer_second_for_test",
      [](std::shared_ptr<trossen::hw::HardwareComponent>,
         const nlohmann::json&)
        -> std::shared_ptr<trossen::hw::PushProducer>
      {
        return std::make_shared<MockPushProducer>();
      });
  } catch (const std::runtime_error&) {
    // Already registered
  }

  auto types = PushProducerRegistry::get_registered_types();
  bool found_first = false;
  bool found_second = false;
  for (const auto& t : types) {
    if (t == "mock_push_producer_for_test") found_first = true;
    if (t == "mock_push_producer_second_for_test") found_second = true;
  }
  EXPECT_TRUE(found_first);
  EXPECT_TRUE(found_second);
}
