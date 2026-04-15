/**
 * @file test_teleop_space_helpers.cpp
 * @brief Unit tests for the teleop space descriptor table and helpers.
 *
 * Locks down the mapping between Space enum values, JSON names, and C++
 * interface names. These helpers drive user-visible configuration parsing
 * and error messages, so silent drift here breaks real configs.
 */

#include <optional>
#include <string>

#include "gtest/gtest.h"

#include "trossen_sdk/hw/teleop/teleop_capable.hpp"

namespace {

using trossen::hw::teleop::space_from_name;
using trossen::hw::teleop::space_iface_name;
using trossen::hw::teleop::space_name;
using Space = trossen::hw::teleop::TeleopCapable::Space;

// space_name: every Space value returns the expected JSON name.
TEST(TeleopSpaceHelpersTest, SpaceNameReturnsLowerCaseJsonName) {
  EXPECT_EQ(space_name(Space::Joint),     "joint");
  EXPECT_EQ(space_name(Space::Cartesian), "cartesian");
}

// space_iface_name: every Space value returns the expected C++ interface name.
TEST(TeleopSpaceHelpersTest, IfaceNameReturnsCppClassName) {
  EXPECT_EQ(space_iface_name(Space::Joint),     "JointSpaceTeleop");
  EXPECT_EQ(space_iface_name(Space::Cartesian), "CartesianSpaceTeleop");
}

// space_from_name: known names resolve to the matching Space value.
TEST(TeleopSpaceHelpersTest, FromNameResolvesKnownSpaces) {
  auto joint = space_from_name("joint");
  ASSERT_TRUE(joint.has_value());
  EXPECT_EQ(*joint, Space::Joint);

  auto cart = space_from_name("cartesian");
  ASSERT_TRUE(cart.has_value());
  EXPECT_EQ(*cart, Space::Cartesian);
}

// space_from_name: unknown inputs return nullopt, never fall through silently.
TEST(TeleopSpaceHelpersTest, FromNameReturnsNulloptForUnknown) {
  EXPECT_FALSE(space_from_name("velocity").has_value());
  EXPECT_FALSE(space_from_name("JOINT").has_value());  // case-sensitive
  EXPECT_FALSE(space_from_name("").has_value());
  EXPECT_FALSE(space_from_name(" joint ").has_value());  // no trimming
}

// Round-trip: for every known space, name → Space → name returns the same name.
TEST(TeleopSpaceHelpersTest, RoundTripNameThroughEnum) {
  for (const auto s : {Space::Joint, Space::Cartesian}) {
    auto name = space_name(s);
    auto parsed = space_from_name(std::string{name});
    ASSERT_TRUE(parsed.has_value()) << "Failed to parse " << name;
    EXPECT_EQ(*parsed, s);
  }
}

}  // namespace
