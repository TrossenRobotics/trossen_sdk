/**
 * @file test_config.cpp
 * @brief Unit tests for configuration system: SessionManagerConfig, GlobalConfig, ConfigRegistry
 *
 * Tests JSON parsing, default values, partial configs, type safety of get_as,
 * and nested namespace handling in GlobalConfig.
 */

#include <memory>
#include <string>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/base_config.hpp"
#include "trossen_sdk/configuration/config_registry.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/types/runtime/session_manager_config.hpp"

using trossen::configuration::BaseConfig;
using trossen::configuration::ConfigRegistry;
using trossen::configuration::GlobalConfig;
using trossen::configuration::SessionManagerConfig;

// ============================================================================
// CFG-01: SessionManagerConfig defaults
// ============================================================================

TEST(SessionManagerConfigTest, Defaults) {
  SessionManagerConfig cfg;

  // Default max_duration is 20 seconds
  ASSERT_TRUE(cfg.max_duration.has_value());
  EXPECT_DOUBLE_EQ(cfg.max_duration->count(), 20.0);

  // Default max_episodes is nullopt (unlimited)
  EXPECT_FALSE(cfg.max_episodes.has_value());

  // Default backend_type
  EXPECT_EQ(cfg.backend_type, "trossen_mcap");

  // Type string
  EXPECT_EQ(cfg.type(), "session_manager");
}

// ============================================================================
// CFG-02: SessionManagerConfig from_json overrides all fields
// ============================================================================

TEST(SessionManagerConfigTest, FromJson_Override) {
  nlohmann::json j = {
    {"type", "session_manager"},
    {"max_duration", 30.0},
    {"max_episodes", 50},
    {"backend_type", "null"}
  };

  SessionManagerConfig cfg = SessionManagerConfig::from_json(j);

  ASSERT_TRUE(cfg.max_duration.has_value());
  EXPECT_DOUBLE_EQ(cfg.max_duration->count(), 30.0);

  ASSERT_TRUE(cfg.max_episodes.has_value());
  EXPECT_EQ(cfg.max_episodes.value(), 50);

  EXPECT_EQ(cfg.backend_type, "null");
}

// ============================================================================
// CFG-03: SessionManagerConfig from_json with partial JSON uses defaults
// ============================================================================

TEST(SessionManagerConfigTest, FromJson_Partial) {
  // Only provide type, everything else defaults
  nlohmann::json j = {
    {"type", "session_manager"}
  };

  SessionManagerConfig cfg = SessionManagerConfig::from_json(j);

  // max_duration should still be default 20s
  ASSERT_TRUE(cfg.max_duration.has_value());
  EXPECT_DOUBLE_EQ(cfg.max_duration->count(), 20.0);

  // max_episodes should be nullopt
  EXPECT_FALSE(cfg.max_episodes.has_value());

  // backend_type should be default
  EXPECT_EQ(cfg.backend_type, "trossen_mcap");
}

// ============================================================================
// CFG-04: GlobalConfig load and retrieve
// ============================================================================

TEST(GlobalConfigTest, LoadAndRetrieve) {
  nlohmann::json config = {
    {"session_manager", {
      {"type", "session_manager"},
      {"max_duration", 15.0},
      {"backend_type", "null"}
    }}
  };

  // NOTE: This modifies the global singleton. Since other test suites
  // also load configs, we accept that the last load wins.
  GlobalConfig::instance().load_from_json(config);

  auto sm_cfg = GlobalConfig::instance().get_as<SessionManagerConfig>("session_manager");
  ASSERT_NE(sm_cfg, nullptr);
  EXPECT_DOUBLE_EQ(sm_cfg->max_duration->count(), 15.0);
  EXPECT_EQ(sm_cfg->backend_type, "null");
}

// ============================================================================
// CFG-06: GlobalConfig get for missing key returns nullptr
// ============================================================================

TEST(GlobalConfigTest, Get_MissingKey_ReturnsNull) {
  auto result = GlobalConfig::instance().get("nonexistent_key_xyz");
  EXPECT_EQ(result, nullptr);
}

// ============================================================================
// CFG-05: GlobalConfig get_as with wrong type throws
// ============================================================================

// A different config type for testing type mismatch.
// Used to verify get_as<T> throws when T does not match the stored config type.
struct DummyConfig : public BaseConfig {
  std::string type() const override { return "dummy"; }
};

TEST(GlobalConfigTest, GetAs_WrongType_Throws) {
  // Ensure session_manager is loaded (from previous test or fixture)
  nlohmann::json config = {
    {"session_manager", {
      {"type", "session_manager"},
      {"max_duration", 10.0},
      {"backend_type", "null"}
    }}
  };
  GlobalConfig::instance().load_from_json(config);

  // Try to get session_manager as DummyConfig -- should throw
  EXPECT_THROW(
    GlobalConfig::instance().get_as<DummyConfig>("session_manager"),
    std::runtime_error);
}

// ============================================================================
// ConfigRegistry: unknown type throws
// ============================================================================

TEST(ConfigRegistryTest, UnknownType_Throws) {
  nlohmann::json j = {{"type", "completely_unknown_type_xyz"}};
  EXPECT_THROW(
    ConfigRegistry::instance().create(j),
    std::runtime_error);
}

// ============================================================================
// ConfigRegistry: session_manager type is registered
// ============================================================================

TEST(ConfigRegistryTest, SessionManagerType_Registered) {
  nlohmann::json j = {
    {"type", "session_manager"},
    {"max_duration", 5.0},
    {"backend_type", "null"}
  };

  auto cfg = ConfigRegistry::instance().create(j);
  ASSERT_NE(cfg, nullptr);
  EXPECT_EQ(cfg->type(), "session_manager");

  // Verify it can be downcast
  auto sm_cfg = std::dynamic_pointer_cast<SessionManagerConfig>(cfg);
  ASSERT_NE(sm_cfg, nullptr);
  EXPECT_DOUBLE_EQ(sm_cfg->max_duration->count(), 5.0);
}
