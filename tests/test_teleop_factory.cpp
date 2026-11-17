/**
 * @file test_teleop_factory.cpp
 * @brief Unit tests for create_controllers_from_global_config() pair validation.
 */

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"
#include "trossen_sdk/hw/teleop/teleop_factory.hpp"

namespace {

using trossen::hw::ActiveHardwareRegistry;
using trossen::hw::HardwareComponent;
using trossen::hw::teleop::JointSpaceTeleop;
using trossen::hw::teleop::create_controllers_from_global_config;

/// A component that can take part in joint-space teleop, such as an arm.
class FakeTeleopComponent : public HardwareComponent, public JointSpaceTeleop {
public:
  explicit FakeTeleopComponent(const std::string& id) : HardwareComponent(id) {}
  void configure(const nlohmann::json&) override {}
  std::string get_type() const override { return "fake_teleop"; }
  std::vector<float> read() override { return {0.0f}; }
  void write(const std::vector<float>&) override {}
};

/// A component that cannot take part in teleop, such as a camera.
class FakePlainComponent : public HardwareComponent {
public:
  explicit FakePlainComponent(const std::string& id) : HardwareComponent(id) {}
  void configure(const nlohmann::json&) override {}
  std::string get_type() const override { return "fake_plain"; }
};

class TeleopFactoryTest : public ::testing::Test {
protected:
  void SetUp() override { ActiveHardwareRegistry::clear(); }
  void TearDown() override { ActiveHardwareRegistry::clear(); }

  /// Loads a teleop config with one pair into the global config.
  /// @param leader Leader component id.
  /// @param follower Follower component id.
  /// @param space Teleop space name.
  static void load_pair(
    const std::string& leader, const std::string& follower, const std::string& space)
  {
    trossen::configuration::GlobalConfig::instance().load_from_json({
      {"teleop", {
        {"type", "teleop"},
        {"enabled", true},
        {"rate_hz", 1000.0},
        {"pairs", {{{"leader", leader}, {"follower", follower}, {"space", space}}}},
      }},
    });
  }
};

// A pair with no follower builds a leader-only controller.
TEST_F(TeleopFactoryTest, PairWithoutFollowerBuildsLeaderOnly) {
  ActiveHardwareRegistry::register_active(
    "leader", std::make_shared<FakeTeleopComponent>("leader"));
  load_pair("leader", "", "joint");

  const auto controllers = create_controllers_from_global_config();
  ASSERT_EQ(controllers.size(), 1u);
  EXPECT_EQ(controllers[0]->follower(), nullptr);
}

// A pair whose leader id is not registered throws, naming the leader.
TEST_F(TeleopFactoryTest, UnknownLeaderThrows) {
  ActiveHardwareRegistry::register_active(
    "follower", std::make_shared<FakeTeleopComponent>("follower"));
  load_pair("leader_typo", "follower", "joint");

  try {
    create_controllers_from_global_config();
    ADD_FAILURE() << "an unknown leader was accepted";
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(std::string(e.what()).find("leader 'leader_typo'"), std::string::npos) << e.what();
  }
}

// A pair whose follower id is not registered throws instead of running leader-only.
TEST_F(TeleopFactoryTest, UnknownFollowerThrows) {
  ActiveHardwareRegistry::register_active(
    "leader", std::make_shared<FakeTeleopComponent>("leader"));
  load_pair("leader", "follower_typo", "joint");

  try {
    create_controllers_from_global_config();
    ADD_FAILURE() << "an unknown follower was accepted";
  } catch (const std::invalid_argument& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("teleop pair 'leader' -> 'follower_typo'"), std::string::npos) << what;
    EXPECT_NE(what.find("follower 'follower_typo' is not registered"), std::string::npos)
      << what;
  }
}

// A pair whose follower cannot be teleoperated (a camera) throws, naming the follower.
TEST_F(TeleopFactoryTest, NonTeleopFollowerThrows) {
  ActiveHardwareRegistry::register_active(
    "leader", std::make_shared<FakeTeleopComponent>("leader"));
  ActiveHardwareRegistry::register_active(
    "camera", std::make_shared<FakePlainComponent>("camera"));
  load_pair("leader", "camera", "joint");

  try {
    create_controllers_from_global_config();
    ADD_FAILURE() << "a follower that cannot be teleoperated was accepted";
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(std::string(e.what()).find("follower 'camera'"), std::string::npos) << e.what();
  }
}

// A misspelled space throws, naming the pair and the space.
TEST_F(TeleopFactoryTest, UnknownSpaceThrowsNamingThePair) {
  ActiveHardwareRegistry::register_active(
    "leader", std::make_shared<FakeTeleopComponent>("leader"));
  ActiveHardwareRegistry::register_active(
    "follower", std::make_shared<FakeTeleopComponent>("follower"));
  load_pair("leader", "follower", "jiont");

  try {
    create_controllers_from_global_config();
    ADD_FAILURE() << "an unknown space was accepted";
  } catch (const std::invalid_argument& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("teleop pair 'leader' -> 'follower'"), std::string::npos) << what;
    EXPECT_NE(what.find("unknown space 'jiont'; use one of: joint, cartesian"), std::string::npos)
      << what;
  }
}

}  // namespace
