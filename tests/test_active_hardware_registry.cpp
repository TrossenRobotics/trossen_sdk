/**
 * @file test_active_hardware_registry.cpp
 * @brief Unit tests for the ActiveHardwareRegistry singleton.
 */

#include <memory>
#include <stdexcept>
#include <string>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"

namespace {

using trossen::hw::ActiveHardwareRegistry;
using trossen::hw::HardwareComponent;

class StubComponent : public HardwareComponent {
 public:
  explicit StubComponent(const std::string& id) : HardwareComponent(id) {}
  void configure(const nlohmann::json&) override {}
  std::string get_type() const override { return "stub"; }
};

class ActiveHardwareRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override { ActiveHardwareRegistry::clear(); }
  void TearDown() override { ActiveHardwareRegistry::clear(); }
};

TEST_F(ActiveHardwareRegistryTest, RegisterAndGet) {
  auto c = std::make_shared<StubComponent>("a");
  ActiveHardwareRegistry::register_active("a", c);
  EXPECT_TRUE(ActiveHardwareRegistry::is_registered("a"));
  EXPECT_EQ(ActiveHardwareRegistry::get("a"), c);
}

TEST_F(ActiveHardwareRegistryTest, DuplicateRegistrationThrows) {
  ActiveHardwareRegistry::register_active(
    "a", std::make_shared<StubComponent>("a"));
  EXPECT_THROW(
    ActiveHardwareRegistry::register_active(
      "a", std::make_shared<StubComponent>("a")),
    std::runtime_error);
}

TEST_F(ActiveHardwareRegistryTest, UnregisterRemovesEntry) {
  ActiveHardwareRegistry::register_active(
    "a", std::make_shared<StubComponent>("a"));
  ActiveHardwareRegistry::register_active(
    "b", std::make_shared<StubComponent>("b"));
  EXPECT_EQ(ActiveHardwareRegistry::count(), 2u);

  EXPECT_TRUE(ActiveHardwareRegistry::unregister("a"));
  EXPECT_FALSE(ActiveHardwareRegistry::is_registered("a"));
  EXPECT_TRUE(ActiveHardwareRegistry::is_registered("b"));
  EXPECT_EQ(ActiveHardwareRegistry::count(), 1u);
}

TEST_F(ActiveHardwareRegistryTest, UnregisterUnknownIdReturnsFalse) {
  EXPECT_FALSE(ActiveHardwareRegistry::unregister("missing"));
}

TEST_F(ActiveHardwareRegistryTest, UnregisterFollowedByReregister) {
  ActiveHardwareRegistry::register_active(
    "a", std::make_shared<StubComponent>("a"));
  EXPECT_TRUE(ActiveHardwareRegistry::unregister("a"));
  EXPECT_NO_THROW(
    ActiveHardwareRegistry::register_active(
      "a", std::make_shared<StubComponent>("a")));
}

}  // namespace
