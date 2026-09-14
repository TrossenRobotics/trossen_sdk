/**
 * @file test_lerobot_episode.cpp
 * @brief Unit tests for build_features(): joint naming and the base block.
 */

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/io/backends/lerobot_common/lerobot_episode.hpp"

using trossen::io::backends::AlignedEpisode;
using trossen::io::backends::build_features;
using trossen::io::backends::episode_action_dim;
using trossen::io::backends::episode_obs_dim;

namespace {

/// One WidowX AI arm: six joints plus the gripper carriage.
constexpr int kJoints = 7;

AlignedEpisode make_episode(const std::vector<std::string>& leaders,
                            const std::vector<std::string>& followers) {
  AlignedEpisode ep;
  ep.leader_streams = leaders;
  ep.follower_streams = followers;
  ep.joints_per_stream = kJoints;
  return ep;
}

/// The names the lerobot_trossen WidowX AI classes report for one arm.
std::vector<std::string> arm_names(const std::string& prefix) {
  std::vector<std::string> names;
  for (int i = 0; i < kJoints - 1; ++i) {
    names.push_back(prefix + "joint_" + std::to_string(i) + ".pos");
  }
  names.push_back(prefix + "left_carriage_joint.pos");
  return names;
}

std::vector<std::string> names_of(const nlohmann::ordered_json& feature) {
  return feature.at("names").get<std::vector<std::string>>();
}

std::vector<std::string> concat(std::vector<std::string> a, const std::vector<std::string>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

}  // namespace

TEST(BuildFeaturesTest, BimanualNamesCarryTheSide) {
  const auto ep =
    make_episode({"leader_left", "leader_right"}, {"follower_left", "follower_right"});
  const auto features = build_features(ep);

  const auto expected = concat(arm_names("left_"), arm_names("right_"));
  EXPECT_EQ(names_of(features["observation.state"]), expected);
  EXPECT_EQ(names_of(features["action"]), expected);
  EXPECT_EQ(features["observation.state"]["shape"][0].get<int>(), 2 * kJoints);
  EXPECT_EQ(features["action"]["shape"][0].get<int>(), 2 * kJoints);
}

TEST(BuildFeaturesTest, SingleArmNamesHaveNoPrefix) {
  const auto ep = make_episode({"leader"}, {"follower"});
  const auto features = build_features(ep);

  EXPECT_EQ(names_of(features["observation.state"]), arm_names(""));
  EXPECT_EQ(names_of(features["action"]), arm_names(""));
}

TEST(BuildFeaturesTest, MobileBaseAppendsVelocities) {
  auto ep = make_episode({"leader_left"}, {"follower_left"});
  ep.has_mobile_base = true;
  ep.dataset_info["mobile_base"]["velocity_names"] = {"linear_vel", "angular_vel"};
  const auto features = build_features(ep);

  const auto expected = concat(arm_names("left_"), {"linear_vel", "angular_vel"});
  EXPECT_EQ(names_of(features["observation.state"]), expected);
  EXPECT_EQ(names_of(features["action"]), expected);
  EXPECT_EQ(features["observation.state"]["shape"][0].get<int>(), episode_obs_dim(ep));
  EXPECT_EQ(features["action"]["shape"][0].get<int>(), episode_action_dim(ep));
}

TEST(BuildFeaturesTest, RecordedBaseVelocityNamesAreUsed) {
  auto ep = make_episode({"leader_left"}, {"follower_left"});
  ep.has_mobile_base = true;
  ep.dataset_info["mobile_base"]["velocity_names"] = {"vx", "wz"};
  const auto features = build_features(ep);
  EXPECT_EQ(names_of(features["action"]), concat(arm_names("left_"), {"vx", "wz"}));
}
