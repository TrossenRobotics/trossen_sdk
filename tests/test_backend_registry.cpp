/**
 * @file test_backend_registry.cpp
 * @brief Unit tests for BackendRegistry
 */

#include <memory>
#include <stdexcept>

#include "gtest/gtest.h"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/io/backend_registry.hpp"
#include "trossen_sdk/io/backends/null/null_backend.hpp"
#include "trossen_sdk/io/backends/mcap/mcap_backend.hpp"

using trossen::io::BackendRegistry;
using trossen::io::Backend;
using trossen::io::backends::NullBackend;
using trossen::io::backends::McapBackend;

// Test that common backend types are registered
TEST(BackendRegistryTest, CommonBackendsAreRegistered) {
  EXPECT_TRUE(BackendRegistry::is_registered("lerobot"));
  EXPECT_TRUE(BackendRegistry::is_registered("mcap"));
  EXPECT_TRUE(BackendRegistry::is_registered("null"));
  EXPECT_TRUE(BackendRegistry::is_registered("trossen"));
}

// Test that unknown backend types are not registered
TEST(BackendRegistryTest, UnknownBackendNotRegistered) {
  EXPECT_FALSE(BackendRegistry::is_registered("unknown"));
  EXPECT_FALSE(BackendRegistry::is_registered(""));
}

// Test creating a NullBackend through the registry
TEST(BackendRegistryTest, CreateNullBackend) {
  NullBackend::Config cfg;
  cfg.type = "null";
  cfg.uri = "null://test";

  auto backend = BackendRegistry::create("null", cfg);
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

// Test creating an McapBackend through the registry
TEST(BackendRegistryTest, CreateMcapBackend) {
  McapBackend::Config cfg;
  cfg.type = "mcap";
  cfg.root = "/tmp/test_registry.mcap";
  cfg.robot_name = "test_robot";
  cfg.dataset_id = "test_dataset";
  cfg.episode_index = 0;

  auto backend = BackendRegistry::create("mcap", cfg);
  ASSERT_NE(backend, nullptr);

  // Verify it's actually an McapBackend
  auto* mcap_backend = dynamic_cast<McapBackend*>(backend.get());
  ASSERT_NE(mcap_backend, nullptr);
}

// Test registry with different backend configurations
TEST(BackendRegistryTest, MultipleBackendsWithDifferentConfigs) {
  // Create first null backend
  NullBackend::Config cfg1;
  cfg1.type = "null";
  cfg1.uri = "null://instance1";
  auto backend1 = BackendRegistry::create("null", cfg1);
  ASSERT_NE(backend1, nullptr);

  // Create second null backend with different config
  NullBackend::Config cfg2;
  cfg2.type = "null";
  cfg2.uri = "null://instance2";
  auto backend2 = BackendRegistry::create("null", cfg2);
  ASSERT_NE(backend2, nullptr);

  // Both should be independent
  EXPECT_NE(backend1.get(), backend2.get());

  // Create mcap backend
  McapBackend::Config cfg3;
  cfg3.type = "mcap";
  cfg3.root = "/tmp/test.mcap";
  auto backend3 = BackendRegistry::create("mcap", cfg3);
  ASSERT_NE(backend3, nullptr);

  // Should be different types
  EXPECT_EQ(dynamic_cast<NullBackend*>(backend3.get()), nullptr);
  EXPECT_EQ(dynamic_cast<McapBackend*>(backend1.get()), nullptr);
}

// Test that base Backend::Config works with proper downcasting
TEST(BackendRegistryTest, ConfigDowncasting) {
  // Create config as concrete type
  NullBackend::Config null_cfg;
  null_cfg.type = "null";
  null_cfg.uri = "null://downcast_test";

  // Pass as base reference (simulating SessionManager usage)
  Backend::Config& base_cfg = null_cfg;

  auto backend = BackendRegistry::create("null", base_cfg);
  ASSERT_NE(backend, nullptr);

  auto* null_backend = dynamic_cast<NullBackend*>(backend.get());
  ASSERT_NE(null_backend, nullptr);
}

// Test polymorphic behavior through registry
TEST(BackendRegistryTest, PolymorphicBackendUsage) {
  NullBackend::Config cfg;
  cfg.type = "null";

  // Create through registry, use through base class interface
  std::shared_ptr<Backend> backend = BackendRegistry::create("null", cfg);

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
TEST(BackendRegistryTest, TypicalUsageDemo) {
  // Simulate selecting backend type at runtime (e.g., from config file)
  std::string backend_type = "null";

  // Create appropriate config based on type
  std::unique_ptr<Backend::Config> cfg;
  if (backend_type == "null") {
    auto null_cfg = std::make_unique<NullBackend::Config>();
    null_cfg->type = "null";
    null_cfg->uri = "null://demo";
    cfg = std::move(null_cfg);
  } else if (backend_type == "mcap") {
    auto mcap_cfg = std::make_unique<McapBackend::Config>();
    mcap_cfg->type = "mcap";
    mcap_cfg->root = "/tmp/demo.mcap";
    cfg = std::move(mcap_cfg);
  }

  ASSERT_NE(cfg, nullptr);

  // Create backend through registry
  auto backend = BackendRegistry::create(backend_type, *cfg);
  ASSERT_NE(backend, nullptr);

  // Use backend polymorphically
  EXPECT_TRUE(backend->open());
  backend->close();
}
