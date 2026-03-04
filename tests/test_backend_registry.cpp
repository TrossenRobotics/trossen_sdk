/**
 * @file test_backend_registry.cpp
 * @brief Unit tests for BackendRegistry
 */

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "gtest/gtest.h"

#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/io/backend_registry.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_backend.hpp"
#include "trossen_sdk/io/backends/null/null_backend.hpp"

using trossen::io::BackendRegistry;
using trossen::io::Backend;
using trossen::io::backends::NullBackend;
using trossen::io::backends::TrossenMCAPBackend;

// Test fixture to load configuration before running tests
class BackendRegistryTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    // Load configuration once for all tests
    // Tests run from build/tests directory, so we need to go up two levels
    const std::string config_path = "../../config/sdk_config.json";

    if (!std::filesystem::exists(config_path)) {
      std::cerr << "Warning: " << config_path << " not found" << std::endl;
      std::cerr << "Current directory: " << std::filesystem::current_path() << std::endl;
      return;
    }

    try {
      auto j = trossen::configuration::JsonLoader::load(config_path);
      trossen::configuration::GlobalConfig::instance().load_from_json(j);
      std::cout << "Successfully loaded configuration from " << config_path << std::endl;
    } catch (const std::exception& e) {
      std::cerr << "Error loading config: " << e.what() << std::endl;
    }
  }
};

// Test that common backend types are registered
TEST_F(BackendRegistryTest, CommonBackendsAreRegistered) {
  EXPECT_TRUE(BackendRegistry::is_registered("lerobot_v2"));
  EXPECT_TRUE(BackendRegistry::is_registered("trossen_mcap"));
  EXPECT_TRUE(BackendRegistry::is_registered("null"));
  EXPECT_TRUE(BackendRegistry::is_registered("trossen"));
}

// Test that unknown backend types are not registered
TEST_F(BackendRegistryTest, UnknownBackendNotRegistered) {
  EXPECT_FALSE(BackendRegistry::is_registered("unknown"));
  EXPECT_FALSE(BackendRegistry::is_registered(""));
}

// Test creating a NullBackend through the registry
TEST_F(BackendRegistryTest, CreateNullBackend) {
  auto backend = BackendRegistry::create("null");
  ASSERT_NE(backend, nullptr);

  // Verify it works as expected
  EXPECT_TRUE(backend->open());

  // Write a test record
  trossen::data::JointStateRecord record;
  record.ts = trossen::data::make_timestamp_now();
  record.seq = 1;
  record.id = "test/joints";
  record.positions = {1.0f, 2.0f, 3.0f};

  backend->write(record);
  backend->flush();
  backend->close();

  // Verify it's actually a NullBackend
  auto* null_backend = dynamic_cast<NullBackend*>(backend.get());
  ASSERT_NE(null_backend, nullptr);
  EXPECT_EQ(null_backend->count(), 1);
}

// Test creating a TrossenMCAPBackend through the registry
TEST_F(BackendRegistryTest, CreateTrossenMCAPBackend) {
  auto backend = BackendRegistry::create("trossen_mcap");
  ASSERT_NE(backend, nullptr);

  // Verify it's actually a TrossenMCAPBackend
  auto* mcap_backend = dynamic_cast<TrossenMCAPBackend*>(backend.get());
  ASSERT_NE(mcap_backend, nullptr);
}

// Test registry with different backend configurations
TEST_F(BackendRegistryTest, MultipleBackendsWithDifferentConfigs) {
  // Create first null backend
  auto backend1 = BackendRegistry::create("null");
  ASSERT_NE(backend1, nullptr);

  // Create second null backend with different config
  auto backend2 = BackendRegistry::create("null");
  ASSERT_NE(backend2, nullptr);

  // Both should be independent
  EXPECT_NE(backend1.get(), backend2.get());

  auto backend3 = BackendRegistry::create("trossen_mcap");
  ASSERT_NE(backend3, nullptr);

  // Should be different types
  EXPECT_EQ(dynamic_cast<NullBackend*>(backend3.get()), nullptr);
  EXPECT_EQ(dynamic_cast<TrossenMCAPBackend*>(backend1.get()), nullptr);
}

// Test that base Backend::Config works with proper downcasting
TEST_F(BackendRegistryTest, ConfigDowncasting) {
  auto backend = BackendRegistry::create("null");
  ASSERT_NE(backend, nullptr);

  auto* null_backend = dynamic_cast<NullBackend*>(backend.get());
  ASSERT_NE(null_backend, nullptr);
}

// Test polymorphic behavior through registry
TEST_F(BackendRegistryTest, PolymorphicBackendUsage) {
  // Create through registry, use through base class interface
  std::shared_ptr<Backend> backend = BackendRegistry::create("null");

  // All backends should support these operations
  EXPECT_TRUE(backend->open());

  trossen::data::JointStateRecord record;
  record.ts = trossen::data::make_timestamp_now();
  record.seq = 1;
  record.id = "test";

  backend->write(record);
  backend->flush();
  backend->close();
}

// Demo test showing typical usage pattern
TEST_F(BackendRegistryTest, TypicalUsageDemo) {
  // Simulate selecting backend type at runtime (e.g., from config file)
  std::string backend_type = "null";


  // Create backend through registry
  auto backend = BackendRegistry::create(backend_type);
  ASSERT_NE(backend, nullptr);

  // Use backend polymorphically
  EXPECT_TRUE(backend->open());
  backend->close();
}
