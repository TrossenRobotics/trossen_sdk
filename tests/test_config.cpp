/**
 * @file test_config.cpp
 * @brief Unit tests for configuration system: SessionManagerConfig, GlobalConfig, ConfigRegistry
 *
 * Tests JSON parsing, default values, partial configs, type safety of get_as,
 * and nested namespace handling in GlobalConfig.
 */

#include <cmath>
#include <limits>
#include <memory>
#include <string>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/base_config.hpp"
#include "trossen_sdk/configuration/config_registry.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/sdk_config.hpp"
#include "trossen_sdk/configuration/types/hardware/arm_config.hpp"
#include "trossen_sdk/configuration/types/runtime/session_manager_config.hpp"

using trossen::configuration::ArmConfig;
using trossen::configuration::BaseConfig;
using trossen::configuration::ConfigRegistry;
using trossen::configuration::GlobalConfig;
using trossen::configuration::SdkConfig;
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
  // also load configs, subsequent loads merge into the existing config:
  // keys in this JSON overwrite existing ones, but unspecified keys may persist.
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

// ============================================================================
// CFG-08: ArmConfig parses optional staging fields from JSON
// ============================================================================

TEST(ArmConfigTest, FromJson_ParsesStagingFields) {
  nlohmann::json j = {
    {"ip_address", "192.168.1.5"},
    {"model", "wxai_v0"},
    {"end_effector", "wxai_v0_leader"},
    {"staged_position", {0.0f, 1.0f, 0.5f, 0.6f, 0.0f, 0.0f, 0.0f}},
    {"staging_time_s", 3.5f}
  };

  ArmConfig cfg = ArmConfig::from_json(j);

  EXPECT_EQ(cfg.ip_address, "192.168.1.5");
  EXPECT_EQ(cfg.end_effector, "wxai_v0_leader");
  ASSERT_EQ(cfg.staged_position.size(), 7u);
  EXPECT_FLOAT_EQ(cfg.staged_position[1], 1.0f);
  EXPECT_FLOAT_EQ(cfg.staging_time_s, 3.5f);
}

// ============================================================================
// CFG-09: ArmConfig round-trips staging fields through to_json/from_json
// ============================================================================

TEST(ArmConfigTest, RoundTrip_PreservesStagingFields) {
  ArmConfig original;
  original.ip_address = "192.168.1.7";
  original.staged_position = {0.1f, 0.2f, 0.3f};
  original.staging_time_s = 1.25f;

  ArmConfig restored = ArmConfig::from_json(original.to_json());

  EXPECT_EQ(restored.ip_address, "192.168.1.7");
  ASSERT_EQ(restored.staged_position.size(), 3u);
  EXPECT_FLOAT_EQ(restored.staged_position[2], 0.3f);
  EXPECT_FLOAT_EQ(restored.staging_time_s, 1.25f);
}

// ============================================================================
// CFG-10: ArmConfig with no staging omits staged_position in JSON
// ============================================================================

TEST(ArmConfigTest, ToJson_OmitsEmptyStagedPosition) {
  ArmConfig cfg;  // default: empty staged_position

  nlohmann::json j = cfg.to_json();

  // staged_position must be absent (a present-but-empty array would be
  // rejected by TrossenArmComponent::configure()), while staging_time_s
  // is always emitted.
  EXPECT_FALSE(j.contains("staged_position"));
  EXPECT_TRUE(j.contains("staging_time_s"));
  EXPECT_FLOAT_EQ(j.at("staging_time_s").get<float>(), 2.0f);
}

// ============================================================================
// CFG-11: ArmConfig command smoothing is off unless explicitly enabled
// ============================================================================

TEST(ArmConfigTest, Smoothing_DefaultsOffAndIsOmittedFromJson) {
  ArmConfig cfg;

  // Off by default: smoothing trades lag for jitter rejection, which is only
  // worth it for an arm mirroring a hand-held leader.
  EXPECT_FALSE(cfg.smoothing_enabled);
  EXPECT_FALSE(cfg.smoothing_gripper);

  // While disabled the tuning is meaningless, so it stays out of the JSON to
  // keep ordinary arm configs clean (same rule as the gripper-feedback block).
  nlohmann::json j = cfg.to_json();
  EXPECT_FALSE(j.contains("smoothing_enabled"));
  EXPECT_FALSE(j.contains("smoothing_beta"));
}

// ============================================================================
// CFG-12: ArmConfig round-trips smoothing tuning when enabled
// ============================================================================

TEST(ArmConfigTest, Smoothing_RoundTripsWhenEnabled) {
  ArmConfig original;
  original.smoothing_enabled = true;
  original.smoothing_gripper = true;
  original.smoothing_min_cutoff_hz = 2.5f;
  original.smoothing_beta = 0.4f;
  original.smoothing_d_cutoff_hz = 1.5f;

  ArmConfig restored = ArmConfig::from_json(original.to_json());

  EXPECT_TRUE(restored.smoothing_enabled);
  EXPECT_TRUE(restored.smoothing_gripper);
  EXPECT_FLOAT_EQ(restored.smoothing_min_cutoff_hz, 2.5f);
  EXPECT_FLOAT_EQ(restored.smoothing_beta, 0.4f);
  EXPECT_FLOAT_EQ(restored.smoothing_d_cutoff_hz, 1.5f);
}

// ============================================================================
// CFG-13: ArmConfig smoothing parses independently of the enable flag
// ============================================================================

TEST(ArmConfigTest, Smoothing_TuningParsesWhileDisabled) {
  // Tuning present but the feature left off: the values must still land, so
  // flipping smoothing_enabled on later does not silently fall back to
  // defaults. Mirrors TrossenArmComponent::configure(), which validates the
  // tuning whether or not smoothing is currently enabled.
  nlohmann::json j = {
    {"ip_address", "192.168.1.9"},
    {"smoothing_beta", 0.75f},
    {"smoothing_min_cutoff_hz", 3.0f}
  };

  ArmConfig cfg = ArmConfig::from_json(j);

  EXPECT_FALSE(cfg.smoothing_enabled);
  EXPECT_FLOAT_EQ(cfg.smoothing_beta, 0.75f);
  EXPECT_FLOAT_EQ(cfg.smoothing_min_cutoff_hz, 3.0f);
}

// ── hardware.components: registry-resolved generic components ─────────────
//
// The escape hatch from a typed config map per component type. Anything the SDK
// does not read fields off directly is declared here and configures itself, so a
// new REGISTER_HARDWARE type costs no schema change.

TEST(HardwareComponentsConfigTest, ParsesIdAndTypeAndKeepsRawJson) {
  const auto j = nlohmann::json::parse(R"({
    "components": [
      { "id": "base_leader", "type": "glide_base",
        "translation": { "arm_id": "glide_left", "max": 0.6 } }
    ]
  })");

  const auto hw = trossen::configuration::HardwareConfig::from_json(j);
  ASSERT_EQ(hw.components.size(), 1u);
  EXPECT_EQ(hw.components[0].id, "base_leader");
  EXPECT_EQ(hw.components[0].type, "glide_base");

  // The component's own fields must survive untouched — the config layer has no
  // business knowing what "translation" means.
  EXPECT_EQ(hw.components[0].raw["translation"]["arm_id"], "glide_left");
  EXPECT_DOUBLE_EQ(hw.components[0].raw["translation"]["max"].get<double>(), 0.6);
}

TEST(HardwareComponentsConfigTest, PreservesDeclarationOrder) {
  const auto j = nlohmann::json::parse(R"({
    "components": [
      { "id": "first",  "type": "glide_base" },
      { "id": "second", "type": "glide_session_control" },
      { "id": "third",  "type": "trossen_base" }
    ]
  })");

  // Order matters: a component that resolves another by id needs that one built
  // first, so the list must not be reordered into a map.
  const auto hw = trossen::configuration::HardwareConfig::from_json(j);
  ASSERT_EQ(hw.components.size(), 3u);
  EXPECT_EQ(hw.components[0].id, "first");
  EXPECT_EQ(hw.components[1].id, "second");
  EXPECT_EQ(hw.components[2].id, "third");
}

TEST(HardwareComponentsConfigTest, MissingIdOrTypeIsRejected) {
  EXPECT_THROW(
    trossen::configuration::HardwareConfig::from_json(
      nlohmann::json::parse(R"({"components":[{"type":"glide_base"}]})")),
    std::runtime_error);

  EXPECT_THROW(
    trossen::configuration::HardwareConfig::from_json(
      nlohmann::json::parse(R"({"components":[{"id":"x"}]})")),
    std::runtime_error);

  // Empty strings are rejected too: an empty id can never be referenced by a
  // teleop pair, and an empty type names no registered hardware.
  EXPECT_THROW(
    trossen::configuration::HardwareConfig::from_json(
      nlohmann::json::parse(R"({"components":[{"id":"","type":"glide_base"}]})")),
    std::runtime_error);
}

TEST(HardwareComponentsConfigTest, DuplicateIdWithinComponentsIsRejected) {
  EXPECT_THROW(
    trossen::configuration::HardwareConfig::from_json(nlohmann::json::parse(R"({
      "components": [
        { "id": "dup", "type": "glide_base" },
        { "id": "dup", "type": "trossen_base" }
      ]
    })")),
    std::runtime_error);
}

TEST(HardwareComponentsConfigTest, IdCollidingWithAnArmIsRejected) {
  // Teleop pairs and ActiveHardwareRegistry resolve by id alone, so a collision
  // across maps would silently point a pair at the wrong device.
  EXPECT_THROW(
    trossen::configuration::HardwareConfig::from_json(nlohmann::json::parse(R"({
      "arms": { "shared_name": { "ip_address": "192.168.1.2" } },
      "components": [ { "id": "shared_name", "type": "glide_base" } ]
    })")),
    std::runtime_error);
}

TEST(HardwareComponentsConfigTest, AbsentSectionYieldsNoComponents) {
  const auto hw = trossen::configuration::HardwareConfig::from_json(
    nlohmann::json::parse(R"({"arms":{}})"));
  EXPECT_TRUE(hw.components.empty());
}

// ---------------------------------------------------------------------------
// CFG-14..17: the command clamp (ArmConfig::command_position_min/max)
//
// Separate from position_min/max, which are the arm's operating limits and are
// enforced by the controller. These bound what teleop may ASK for. Because a
// rig usually wants to bound ONE axis (the Rivet clamps J0 only, to keep each
// follower out of the other's space), an entry has to be able to say "leave
// this joint alone" — which JSON can only spell as null.
// ---------------------------------------------------------------------------

TEST(ArmConfigTest, CommandClamp_DefaultsOffAndIsOmittedFromJson) {
  ArmConfig cfg;
  EXPECT_TRUE(cfg.command_position_min.empty());
  EXPECT_TRUE(cfg.command_position_max.empty());
  const auto j = cfg.to_json();
  EXPECT_FALSE(j.contains("command_position_min"));
  EXPECT_FALSE(j.contains("command_position_max"));
}

TEST(ArmConfigTest, CommandClamp_NullMeansUnclamped) {
  const auto j = nlohmann::json::parse(R"({
    "ip_address": "192.168.1.4",
    "command_position_min": [-1.1, null, null, null, null, null, null],
    "command_position_max": [0.8,  null, null, null, null, null, null]
  })");
  const ArmConfig cfg = ArmConfig::from_json(j);
  ASSERT_EQ(cfg.command_position_min.size(), 7u);
  EXPECT_FLOAT_EQ(cfg.command_position_min[0], -1.1f);
  EXPECT_FLOAT_EQ(cfg.command_position_max[0], 0.8f);
  for (size_t i = 1; i < 7; ++i) {
    EXPECT_TRUE(std::isnan(cfg.command_position_min[i])) << "joint " << i;
    EXPECT_TRUE(std::isnan(cfg.command_position_max[i])) << "joint " << i;
  }
}

TEST(ArmConfigTest, CommandClamp_RoundTripsNullsBackToNull) {
  // A NaN written as a bare number would be invalid JSON and nlohmann emits it
  // as null anyway on some paths — pin the representation so a saved config
  // reloads as the same sparse clamp rather than as a clamp at zero.
  ArmConfig original;
  original.command_position_min = {
    -1.1f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()};
  const auto j = original.to_json();
  ASSERT_TRUE(j.contains("command_position_min"));
  EXPECT_TRUE(j["command_position_min"][0].is_number());
  EXPECT_TRUE(j["command_position_min"][1].is_null());

  const ArmConfig restored = ArmConfig::from_json(j);
  ASSERT_EQ(restored.command_position_min.size(), 3u);
  EXPECT_FLOAT_EQ(restored.command_position_min[0], -1.1f);
  EXPECT_TRUE(std::isnan(restored.command_position_min[1]));
}

TEST(ArmConfigTest, CommandClamp_IsIndependentOfTheControllerLimits) {
  // The two must not be conflated: an arm can bound teleop's reach on J0 while
  // leaving its operating limits at the firmware defaults.
  const auto j = nlohmann::json::parse(R"({
    "ip_address": "192.168.1.4",
    "command_position_min": [-1.1, null, null, null, null, null, null]
  })");
  const ArmConfig cfg = ArmConfig::from_json(j);
  EXPECT_FALSE(cfg.command_position_min.empty());
  EXPECT_TRUE(cfg.position_min.empty());
  EXPECT_TRUE(cfg.position_max.empty());
}
