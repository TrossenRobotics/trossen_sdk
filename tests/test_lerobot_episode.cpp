/**
 * @file test_lerobot_episode.cpp
 * @brief Unit tests for build_features(): joint naming and signal blocks.
 */

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/io/backends/lerobot_common/lerobot_episode.hpp"

using trossen::io::backends::AlignedEpisode;
using trossen::io::backends::build_features;
using trossen::io::backends::DatasetSignalOptions;
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
std::vector<std::string> arm_names(const std::string& prefix, const std::string& suffix = ".pos") {
  std::vector<std::string> names;
  for (int i = 0; i < kJoints - 1; ++i) {
    names.push_back(prefix + "joint_" + std::to_string(i) + suffix);
  }
  names.push_back(prefix + "left_carriage_joint" + suffix);
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

TEST(BuildFeaturesTest, VelocityAndEffortFollowEachStreamPositions) {
  const auto ep =
    make_episode({"leader_left", "leader_right"}, {"follower_left", "follower_right"});
  DatasetSignalOptions signals;
  signals.joint_velocity = true;
  signals.joint_effort = true;
  const auto features = build_features(ep, signals);

  std::vector<std::string> expected;
  for (const std::string side : {"left_", "right_"}) {
    expected = concat(expected, arm_names(side));
    expected = concat(expected, arm_names(side, ".vel"));
    expected = concat(expected, arm_names(side, ".eff"));
  }
  EXPECT_EQ(names_of(features["observation.state"]), expected);
  EXPECT_EQ(features["observation.state"]["shape"][0].get<int>(), episode_obs_dim(ep, signals));
  // The action stays positions only.
  EXPECT_EQ(names_of(features["action"]), concat(arm_names("left_"), arm_names("right_")));
}

TEST(BuildFeaturesTest, MobileBaseAppendsVelocities) {
  auto ep = make_episode({"leader_left"}, {"follower_left"});
  ep.has_mobile_base = true;
  const auto features = build_features(ep);

  const auto expected = concat(arm_names("left_"), {"linear_vel", "angular_vel"});
  EXPECT_EQ(names_of(features["observation.state"]), expected);
  EXPECT_EQ(names_of(features["action"]), expected);
  EXPECT_EQ(features["observation.state"]["shape"][0].get<int>(), episode_obs_dim(ep));
  EXPECT_EQ(features["action"]["shape"][0].get<int>(), episode_action_dim(ep));
}

TEST(BuildFeaturesTest, NamesMatchWidthForEverySignalCombination) {
  for (const bool mobile : {false, true}) {
    for (int bits = 0; bits < 16; ++bits) {
      auto ep = make_episode({"leader_left", "leader_right"}, {"follower_left", "follower_right"});
      ep.has_mobile_base = mobile;
      DatasetSignalOptions signals;
      signals.joint_velocity = (bits & 1) != 0;
      signals.joint_effort = (bits & 2) != 0;
      signals.base_lateral_velocity = (bits & 4) != 0;
      signals.base_pose = (bits & 8) != 0;
      const auto features = build_features(ep, signals);

      EXPECT_EQ(names_of(features["observation.state"]).size(),
                static_cast<size_t>(episode_obs_dim(ep, signals)));
      EXPECT_EQ(names_of(features["action"]).size(),
                static_cast<size_t>(episode_action_dim(ep, signals)));
    }
  }
}

TEST(BuildFeaturesTest, BaseBlockFollowsLoaderOrder) {
  auto ep = make_episode({"leader_left"}, {"follower_left"});
  ep.has_mobile_base = true;
  DatasetSignalOptions signals;
  signals.base_lateral_velocity = true;
  signals.base_pose = true;
  const auto features = build_features(ep, signals);

  // load_aligned_episode() appends Twist2D linear_x and angular_z, then linear_y, then the
  // Pose2D x, y and theta, to both the action and the observation rows.
  const auto expected = concat(arm_names("left_"), {"linear_vel", "angular_vel", "lateral_vel",
                                                    "pose_x", "pose_y", "pose_theta"});
  EXPECT_EQ(names_of(features["action"]), expected);
  EXPECT_EQ(names_of(features["observation.state"]), expected);
}

TEST(BuildFeaturesTest, RecordedBaseVelocityNamesAreUsed) {
  auto ep = make_episode({"leader_left"}, {"follower_left"});
  ep.has_mobile_base = true;
  ep.dataset_info["base_velocity_names"] = {"vx", "wz"};
  const auto features = build_features(ep);
  EXPECT_EQ(names_of(features["action"]), concat(arm_names("left_"), {"vx", "wz"}));
}
