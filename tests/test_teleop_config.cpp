/**
 * @file test_teleop_config.cpp
 * @brief Unit tests for TeleoperationConfig and TeleoperationPair parsing.
 */

#include <stdexcept>
#include <string>

#include "gtest/gtest.h"

#include "nlohmann/json.hpp"
#include "trossen_sdk/configuration/types/teleop_config.hpp"

namespace {

using trossen::configuration::TeleoperationConfig;
using trossen::configuration::TeleoperationPair;

TEST(TeleoperationPairTest, DefaultSpaceIsJoint) {
  auto j = nlohmann::json::parse(R"({ "leader": "l", "follower": "f" })");
  auto p = TeleoperationPair::from_json(j);
  EXPECT_EQ(p.leader, "l");
  EXPECT_EQ(p.follower, "f");
  EXPECT_EQ(p.space, "joint");
}

TEST(TeleoperationPairTest, ExplicitSpace) {
  auto j = nlohmann::json::parse(
    R"({ "leader": "l", "follower": "f", "space": "cartesian" })");
  auto p = TeleoperationPair::from_json(j);
  EXPECT_EQ(p.space, "cartesian");
}

TEST(TeleoperationConfigTest, Defaults) {
  TeleoperationConfig c = TeleoperationConfig::from_json(nlohmann::json::object());
  EXPECT_TRUE(c.enabled);
  EXPECT_FLOAT_EQ(c.rate_hz, 1000.0f);
  EXPECT_TRUE(c.pairs.empty());
}

TEST(TeleoperationConfigTest, FullParse) {
  auto j = nlohmann::json::parse(R"({
    "enabled": false,
    "rate_hz": 500.0,
    "pairs": [
      { "leader": "a", "follower": "b", "space": "cartesian" },
      { "leader": "c", "follower": "d" }
    ]
  })");
  auto c = TeleoperationConfig::from_json(j);
  EXPECT_FALSE(c.enabled);
  EXPECT_FLOAT_EQ(c.rate_hz, 500.0f);
  ASSERT_EQ(c.pairs.size(), 2);
  EXPECT_EQ(c.pairs[0].space, "cartesian");
  EXPECT_EQ(c.pairs[1].space, "joint");  // default
}

// A pair without a leader fails at parse, naming its index in "teleop.pairs".
TEST(TeleoperationConfigTest, PairWithoutLeaderThrowsNamingTheIndex) {
  auto j = nlohmann::json::parse(R"({
    "pairs": [
      { "leader": "a", "follower": "b" },
      { "follower": "d" }
    ]
  })");

  try {
    TeleoperationConfig::from_json(j);
    ADD_FAILURE() << "a pair without a leader was accepted";
  } catch (const std::invalid_argument& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("teleop.pairs[1]: 'leader' is required"), std::string::npos) << what;
  }
}

// A rate that is zero or negative fails at parse, naming the key.
TEST(TeleoperationConfigTest, NonPositiveRateThrows) {
  for (const double rate : {0.0, -1.0}) {
    try {
      TeleoperationConfig::from_json({{"rate_hz", rate}});
      ADD_FAILURE() << "rate_hz " << rate << " was accepted";
    } catch (const std::invalid_argument& e) {
      const std::string what = e.what();
      EXPECT_NE(what.find("teleop.rate_hz: must be positive and finite"), std::string::npos)
        << what;
    }
  }
}

}  // namespace
