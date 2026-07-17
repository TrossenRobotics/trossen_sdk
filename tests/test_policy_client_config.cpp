/**
 * @file test_policy_client_config.cpp
 * @brief Unit tests for PolicyClientConfig and HardwareConfig::policy_clients parsing.
 */

#include "gtest/gtest.h"

#include "nlohmann/json.hpp"
#include "trossen_sdk/configuration/sdk_config.hpp"
#include "trossen_sdk/configuration/types/hardware/policy_client_config.hpp"

namespace {

using trossen::configuration::HardwareConfig;
using trossen::configuration::PolicyClientConfig;
using trossen::configuration::PolicyClientJointLayoutEntry;
using trossen::configuration::PolicyClientSubscriptionConfig;

constexpr const char* kCanonicalConfig = R"({
  "id": "policy_main",
  "server_url": "ws://10.0.0.5:8000",
  "inference_hz": 10.0,
  "prompt": "Pick up the red block",
  "subscriptions": [
    { "record_id": "follower_left",  "throttle_hz": 30.0, "obs_key": "state.left"  },
    { "record_id": "follower_right", "throttle_hz": 30.0, "obs_key": "state.right" },
    { "record_id": "cam_high/color", "throttle_hz": 15.0, "obs_key": "images.cam_high",
      "resize": [224, 224] }
  ],
  "joint_layout": [
    { "leader_id": "policy_left",  "joint_offset": 0, "joint_count": 7 },
    { "leader_id": "policy_right", "joint_offset": 7, "joint_count": 7 }
  ]
})";

PolicyClientConfig parse_canonical() {
  return PolicyClientConfig::from_json(nlohmann::json::parse(kCanonicalConfig));
}

// --- PolicyClientSubscriptionConfig ----------------------------------------

TEST(PolicyClientSubscriptionConfigTest, RoundTripCanonical) {
  auto j = nlohmann::json::parse(
    R"({ "record_id": "cam", "throttle_hz": 30.0, "obs_key": "images.x",
        "resize": [224, 224] })");
  auto s = PolicyClientSubscriptionConfig::from_json(j);
  EXPECT_EQ(s.record_id, "cam");
  EXPECT_DOUBLE_EQ(s.throttle_hz, 30.0);
  EXPECT_EQ(s.obs_key, "images.x");
  ASSERT_TRUE(s.resize.has_value());
  EXPECT_EQ(s.resize->first, 224);
  EXPECT_EQ(s.resize->second, 224);
}

TEST(PolicyClientSubscriptionConfigTest, ResizeOptional) {
  auto j = nlohmann::json::parse(
    R"({ "record_id": "joints", "throttle_hz": 30.0, "obs_key": "state.x" })");
  auto s = PolicyClientSubscriptionConfig::from_json(j);
  EXPECT_FALSE(s.resize.has_value());
}

TEST(PolicyClientSubscriptionConfigTest, RejectsEmptyRecordId) {
  auto j = nlohmann::json::parse(
    R"({ "record_id": "", "throttle_hz": 30.0, "obs_key": "state.x" })");
  EXPECT_THROW(PolicyClientSubscriptionConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientSubscriptionConfigTest, RejectsThrottleOutOfRange) {
  auto low = nlohmann::json::parse(
    R"({ "record_id": "r", "throttle_hz": 0.0, "obs_key": "k" })");
  EXPECT_THROW(PolicyClientSubscriptionConfig::from_json(low), std::runtime_error);
  auto high = nlohmann::json::parse(
    R"({ "record_id": "r", "throttle_hz": 1e5, "obs_key": "k" })");
  EXPECT_THROW(PolicyClientSubscriptionConfig::from_json(high), std::runtime_error);
}

TEST(PolicyClientSubscriptionConfigTest, RejectsEmptyObsKey) {
  auto j = nlohmann::json::parse(
    R"({ "record_id": "r", "throttle_hz": 30.0, "obs_key": "" })");
  EXPECT_THROW(PolicyClientSubscriptionConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientSubscriptionConfigTest, RejectsResizeWithWrongShape) {
  auto bad_size = nlohmann::json::parse(
    R"({ "record_id": "r", "throttle_hz": 30.0, "obs_key": "k", "resize": [224] })");
  EXPECT_THROW(
    PolicyClientSubscriptionConfig::from_json(bad_size), std::runtime_error);
}

TEST(PolicyClientSubscriptionConfigTest, RejectsNonPositiveResize) {
  auto zero = nlohmann::json::parse(
    R"({ "record_id": "r", "throttle_hz": 30.0, "obs_key": "k", "resize": [0, 224] })");
  EXPECT_THROW(PolicyClientSubscriptionConfig::from_json(zero), std::runtime_error);
  auto neg = nlohmann::json::parse(
    R"({ "record_id": "r", "throttle_hz": 30.0, "obs_key": "k", "resize": [224, -1] })");
  EXPECT_THROW(PolicyClientSubscriptionConfig::from_json(neg), std::runtime_error);
}

TEST(PolicyClientSubscriptionConfigTest, RejectsNonIntegerResize) {
  auto j = nlohmann::json::parse(
    R"({ "record_id": "r", "throttle_hz": 30.0, "obs_key": "k",
        "resize": [224.5, 224] })");
  EXPECT_THROW(PolicyClientSubscriptionConfig::from_json(j), std::runtime_error);
}

// --- PolicyClientJointLayoutEntry ------------------------------------------

TEST(PolicyClientJointLayoutEntryTest, RoundTripCanonical) {
  auto j = nlohmann::json::parse(
    R"({ "leader_id": "policy_left", "joint_offset": 0, "joint_count": 7 })");
  auto e = PolicyClientJointLayoutEntry::from_json(j);
  EXPECT_EQ(e.leader_id, "policy_left");
  EXPECT_EQ(e.joint_offset, 0);
  EXPECT_EQ(e.joint_count, 7);
}

TEST(PolicyClientJointLayoutEntryTest, RejectsEmptyLeaderId) {
  auto j = nlohmann::json::parse(
    R"({ "leader_id": "", "joint_offset": 0, "joint_count": 7 })");
  EXPECT_THROW(PolicyClientJointLayoutEntry::from_json(j), std::runtime_error);
}

TEST(PolicyClientJointLayoutEntryTest, RejectsNegativeOffset) {
  auto j = nlohmann::json::parse(
    R"({ "leader_id": "x", "joint_offset": -1, "joint_count": 7 })");
  EXPECT_THROW(PolicyClientJointLayoutEntry::from_json(j), std::runtime_error);
}

TEST(PolicyClientJointLayoutEntryTest, RejectsNonPositiveCount) {
  auto zero = nlohmann::json::parse(
    R"({ "leader_id": "x", "joint_offset": 0, "joint_count": 0 })");
  EXPECT_THROW(PolicyClientJointLayoutEntry::from_json(zero), std::runtime_error);
  auto neg = nlohmann::json::parse(
    R"({ "leader_id": "x", "joint_offset": 0, "joint_count": -1 })");
  EXPECT_THROW(PolicyClientJointLayoutEntry::from_json(neg), std::runtime_error);
}

TEST(PolicyClientJointLayoutEntryTest, JointNamesOptionalAndParsed) {
  // Absent: empty (positional fallback downstream).
  auto without = nlohmann::json::parse(
    R"({ "leader_id": "x", "joint_offset": 0, "joint_count": 2 })");
  EXPECT_TRUE(PolicyClientJointLayoutEntry::from_json(without).joint_names.empty());

  // Present with length == joint_count: parsed in order.
  auto with = nlohmann::json::parse(
    R"({ "leader_id": "x", "joint_offset": 0, "joint_count": 2,
         "joint_names": ["waist", "gripper"] })");
  auto e = PolicyClientJointLayoutEntry::from_json(with);
  ASSERT_EQ(e.joint_names.size(), 2u);
  EXPECT_EQ(e.joint_names[0], "waist");
  EXPECT_EQ(e.joint_names[1], "gripper");
}

TEST(PolicyClientJointLayoutEntryTest, RejectsJointNamesLengthMismatch) {
  auto j = nlohmann::json::parse(
    R"({ "leader_id": "x", "joint_offset": 0, "joint_count": 3,
         "joint_names": ["a", "b"] })");
  EXPECT_THROW(PolicyClientJointLayoutEntry::from_json(j), std::runtime_error);
}

TEST(PolicyClientJointLayoutEntryTest, RejectsNonStringJointNames) {
  auto not_array = nlohmann::json::parse(
    R"({ "leader_id": "x", "joint_offset": 0, "joint_count": 2,
         "joint_names": "waist" })");
  EXPECT_THROW(PolicyClientJointLayoutEntry::from_json(not_array), std::runtime_error);

  auto non_string = nlohmann::json::parse(
    R"({ "leader_id": "x", "joint_offset": 0, "joint_count": 2,
         "joint_names": ["waist", 5] })");
  EXPECT_THROW(PolicyClientJointLayoutEntry::from_json(non_string), std::runtime_error);
}

// --- PolicyClientConfig: top-level positive ---------------------------------

TEST(PolicyClientConfigTest, RoundTripCanonical) {
  auto c = parse_canonical();
  EXPECT_EQ(c.id, "policy_main");
  EXPECT_EQ(c.server_url, "ws://10.0.0.5:8000");
  EXPECT_FALSE(c.api_key.has_value());
  EXPECT_DOUBLE_EQ(c.inference_hz, 10.0);
  EXPECT_EQ(c.prompt, "Pick up the red block");
  ASSERT_EQ(c.subscriptions.size(), 3u);
  EXPECT_EQ(c.subscriptions[0].record_id, "follower_left");
  EXPECT_EQ(c.subscriptions[0].obs_key, "state.left");
  EXPECT_TRUE(c.subscriptions[2].resize.has_value());
  ASSERT_EQ(c.joint_layout.size(), 2u);
  EXPECT_EQ(c.joint_layout[0].leader_id, "policy_left");
  EXPECT_EQ(c.joint_layout[1].joint_offset, 7);
  EXPECT_EQ(c.joint_layout[1].joint_count, 7);
  // Transport-selection defaults: existing configs resolve openpi_ws with an
  // empty options object and the synchronous theta=0 firing cadence.
  EXPECT_EQ(c.transport, "openpi_ws");
  EXPECT_TRUE(c.transport_config.is_object());
  EXPECT_TRUE(c.transport_config.empty());
  EXPECT_DOUBLE_EQ(c.drain_threshold, 0.0);
}

TEST(PolicyClientConfigTest, RejectsOverlappingJointLayout) {
  // Two entries whose slices overlap (both cover column 3) sum to a width that
  // can still match the chunk, yet they address the same joints - reject it.
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["joint_layout"] = nlohmann::json::parse(R"([
    { "leader_id": "a", "joint_offset": 0, "joint_count": 7 },
    { "leader_id": "b", "joint_offset": 3, "joint_count": 7 }
  ])");
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientConfigTest, RejectsGappedJointLayout) {
  // A gap between slices (columns 7..9 unowned) leaves part of the action row
  // unaddressed; reject it.
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["joint_layout"] = nlohmann::json::parse(R"([
    { "leader_id": "a", "joint_offset": 0,  "joint_count": 7 },
    { "leader_id": "b", "joint_offset": 10, "joint_count": 7 }
  ])");
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientConfigTest, AcceptsContiguousJointLayoutOutOfOrder) {
  // Order in the array does not matter as long as the slices tile [0, sum).
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["joint_layout"] = nlohmann::json::parse(R"([
    { "leader_id": "b", "joint_offset": 7, "joint_count": 7 },
    { "leader_id": "a", "joint_offset": 0, "joint_count": 7 }
  ])");
  auto c = PolicyClientConfig::from_json(j);
  EXPECT_EQ(c.joint_layout.size(), 2u);
}

TEST(PolicyClientConfigTest, EmptyPromptAllowed) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["prompt"] = "";
  auto c = PolicyClientConfig::from_json(j);
  EXPECT_EQ(c.prompt, "");
}

TEST(PolicyClientConfigTest, OptionalApiKey) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["api_key"] = "secret-token";
  auto c = PolicyClientConfig::from_json(j);
  ASSERT_TRUE(c.api_key.has_value());
  EXPECT_EQ(*c.api_key, "secret-token");
}

TEST(PolicyClientConfigTest, AcceptsWssScheme) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["server_url"] = "wss://example.com:443";
  auto c = PolicyClientConfig::from_json(j);
  EXPECT_EQ(c.server_url, "wss://example.com:443");
}

TEST(PolicyClientConfigTest, AcceptsNonWebsocketUrl) {
  // URL format is transport-specific (a gRPC transport takes host:port), so
  // the shared config no longer enforces a scheme - the selected transport's
  // factory does (openpi_ws rejection is pinned in the transport tests).
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["server_url"] = "10.0.0.5:50051";
  auto c = PolicyClientConfig::from_json(j);
  EXPECT_EQ(c.server_url, "10.0.0.5:50051");
}

TEST(PolicyClientConfigTest, TransportNameParsed) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["transport"] = "lerobot_grpc";
  auto c = PolicyClientConfig::from_json(j);
  EXPECT_EQ(c.transport, "lerobot_grpc");
}

TEST(PolicyClientConfigTest, RejectsEmptyTransportName) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["transport"] = "";
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientConfigTest, TransportConfigPassedVerbatim) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["transport_config"] = {{"api_key", "k"}, {"custom_knob", 3}};
  auto c = PolicyClientConfig::from_json(j);
  ASSERT_TRUE(c.transport_config.is_object());
  EXPECT_EQ(c.transport_config.at("api_key"), "k");
  EXPECT_EQ(c.transport_config.at("custom_knob"), 3);
}

TEST(PolicyClientConfigTest, RejectsNonObjectTransportConfig) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["transport_config"] = "not-an-object";
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientConfigTest, DrainThresholdParsed) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["drain_threshold"] = 0.5;
  auto c = PolicyClientConfig::from_json(j);
  EXPECT_DOUBLE_EQ(c.drain_threshold, 0.5);
}

TEST(PolicyClientConfigTest, RejectsDrainThresholdAtOne) {
  // theta=1 ("fire with the whole chunk unplayed") degenerates to firing always.
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["drain_threshold"] = 1.0;
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientConfigTest, RejectsNegativeDrainThreshold) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["drain_threshold"] = -0.1;
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
}

// --- PolicyClientConfig: validation rule negatives --------------------------

TEST(PolicyClientConfigTest, RejectsEmptyId) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["id"] = "";
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientConfigTest, RejectsMissingServerUrl) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j.erase("server_url");
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientConfigTest, RejectsInferenceHzAtBoundary) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["inference_hz"] = 0.0;
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
  j["inference_hz"] = -1.0;
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
  j["inference_hz"] = 1e4 + 1.0;
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientConfigTest, AcceptsInferenceHzAtUpperBoundary) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["inference_hz"] = 1e4;
  // Each subscription must satisfy throttle_hz >= inference_hz at the new ceiling.
  for (auto& s : j["subscriptions"]) {
    s["throttle_hz"] = 1e4;
  }
  EXPECT_NO_THROW(PolicyClientConfig::from_json(j));
}

TEST(PolicyClientConfigTest, RejectsEmptySubscriptions) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["subscriptions"] = nlohmann::json::array();
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientConfigTest, RejectsMissingSubscriptions) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j.erase("subscriptions");
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientConfigTest, RejectsDuplicateSubscriptionRecordIds) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["subscriptions"][1]["record_id"] = "follower_left";
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientConfigTest, RejectsDuplicateSubscriptionObsKeys) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  // Distinct record_id (follower_right stays), but collide obs_key with subscription[0].
  j["subscriptions"][1]["obs_key"] = "state.left";
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientConfigTest, RejectsThrottleBelowInferenceHz) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  // inference_hz = 10, set one throttle to 5 (still within absolute bounds).
  j["subscriptions"][0]["throttle_hz"] = 5.0;
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientConfigTest, RejectsEmptyJointLayout) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["joint_layout"] = nlohmann::json::array();
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientConfigTest, RejectsMissingJointLayout) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j.erase("joint_layout");
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientConfigTest, RejectsDuplicateLeaderIds) {
  auto j = nlohmann::json::parse(kCanonicalConfig);
  j["joint_layout"][1]["leader_id"] = "policy_left";
  EXPECT_THROW(PolicyClientConfig::from_json(j), std::runtime_error);
}

TEST(PolicyClientConfigTest, RejectsNonObjectInput) {
  EXPECT_THROW(
    PolicyClientConfig::from_json(nlohmann::json::array()), std::runtime_error);
}

TEST(PolicyClientConfigTest, RawJsonRoundTripsParsedInput) {
  const auto j = nlohmann::json::parse(kCanonicalConfig);
  const auto c = PolicyClientConfig::from_json(j);
  EXPECT_EQ(c.raw_json, j);
}

// --- HardwareConfig::policy_clients integration -----------------------------

TEST(HardwareConfigPolicyClientsTest, ParsesArrayWithMultipleClients) {
  nlohmann::json hw_j = nlohmann::json::object();
  hw_j["policy_clients"] = nlohmann::json::array();
  auto a = nlohmann::json::parse(kCanonicalConfig);
  auto b = nlohmann::json::parse(kCanonicalConfig);
  b["id"] = "policy_secondary";
  hw_j["policy_clients"].push_back(a);
  hw_j["policy_clients"].push_back(b);

  auto hw = HardwareConfig::from_json(hw_j);
  ASSERT_EQ(hw.policy_clients.size(), 2u);
  EXPECT_EQ(hw.policy_clients[0].id, "policy_main");
  EXPECT_EQ(hw.policy_clients[1].id, "policy_secondary");
}

TEST(HardwareConfigPolicyClientsTest, AbsentSectionDefaultsToEmpty) {
  auto hw = HardwareConfig::from_json(nlohmann::json::object());
  EXPECT_TRUE(hw.policy_clients.empty());
}

TEST(HardwareConfigPolicyClientsTest, RejectsNonArray) {
  nlohmann::json hw_j = nlohmann::json::object();
  hw_j["policy_clients"] = "not-an-array";
  EXPECT_THROW(HardwareConfig::from_json(hw_j), std::runtime_error);
}

TEST(HardwareConfigPolicyClientsTest, RejectsDuplicateIds) {
  nlohmann::json hw_j = nlohmann::json::object();
  hw_j["policy_clients"] = nlohmann::json::array();
  auto a = nlohmann::json::parse(kCanonicalConfig);
  auto b = nlohmann::json::parse(kCanonicalConfig);
  hw_j["policy_clients"].push_back(a);
  hw_j["policy_clients"].push_back(b);
  EXPECT_THROW(HardwareConfig::from_json(hw_j), std::runtime_error);
}

TEST(HardwareConfigPolicyClientsTest, ReportsIndexOnParseFailure) {
  nlohmann::json hw_j = nlohmann::json::object();
  hw_j["policy_clients"] = nlohmann::json::array();
  hw_j["policy_clients"].push_back(nlohmann::json::parse(kCanonicalConfig));
  // Empty server_url is a parse-level error (scheme checks moved into the
  // transport factories, so a bad scheme no longer throws here).
  auto bad = nlohmann::json::parse(kCanonicalConfig);
  bad["server_url"] = "";
  hw_j["policy_clients"].push_back(bad);
  try {
    HardwareConfig::from_json(hw_j);
    FAIL() << "expected throw";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("policy_clients[1]"), std::string::npos);
  }
}

}  // namespace
