/**
 * @file test_vr_base_component.cpp
 * @brief Unit tests for the VR thumbstick base-velocity teleop leader.
 *
 * Drives the real VrBaseComponent with synthetic frames via the VrSession
 * test seam — no VR hardware required. Covers config validation, the deadzone,
 * scaling, caps, and especially the steering direction (push forward → drive
 * forward, push left → turn left), plus the disconnect and non-finite guards.
 */

#include <cmath>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_vr/vr_types.hpp"

#include "trossen_sdk/hw/vr/vr_base_component.hpp"
#include "trossen_sdk/hw/vr/vr_session.hpp"

using trossen::hw::vr::VrBaseComponent;
using trossen::hw::vr::VrSession;

namespace {

trossen_vr::VRFrame make_stick_frame(const std::string& side, float x_axis,
                                     float y_axis) {
  trossen_vr::VRFrame f;
  auto& c = (side == "right") ? f.right_controller : f.left_controller;
  c.is_tracked = 1;
  c.thumbstick.x_axis = x_axis;
  c.thumbstick.y_axis = y_axis;
  return f;
}

nlohmann::json base_config() {
  return nlohmann::json{
    {"controller_type", "left"},
    {"max_linear_mps", 0.5},
    {"max_angular_rps", 1.0},
    {"deadzone", 0.1},
  };
}

}  // namespace

// ── config validation (no frames needed) ────────────────────────────────────

TEST(VrBaseConfig, MissingAllControllerTypesThrows) {
  VrBaseComponent base("base_cfg1");
  EXPECT_THROW(base.configure(nlohmann::json::object()), std::runtime_error);
}

TEST(VrBaseConfig, InvalidControllerTypeThrows) {
  VrBaseComponent base("base_cfg2");
  EXPECT_THROW(base.configure({{"controller_type", "middle"}}),
               std::runtime_error);
}

TEST(VrBaseConfig, NegativeMaxLinearThrows) {
  VrBaseComponent base("base_cfg3");
  EXPECT_THROW(
    base.configure({{"controller_type", "left"}, {"max_linear_mps", -0.5}}),
    std::runtime_error);
}

TEST(VrBaseConfig, DeadzoneOutOfRangeThrows) {
  VrBaseComponent base("base_cfg4");
  EXPECT_THROW(
    base.configure({{"controller_type", "left"}, {"deadzone", 1.0}}),
    std::runtime_error);
}

TEST(VrBaseConfig, NegativeConnectionTimeoutThrows) {
  VrBaseComponent base("base_cfg5");
  EXPECT_THROW(
    base.configure({{"controller_type", "left"},
                    {"connection_timeout_s", -1.0}}),
    std::runtime_error);
}

// ── velocity mapping (seam-driven) ──────────────────────────────────────────

class VrBaseRead : public ::testing::Test {
 protected:
  void SetUp() override {
    VrSession::instance().set_test_frame(trossen_vr::VRFrame{});
  }
  void TearDown() override { VrSession::instance().clear_test_frame(); }
};

TEST_F(VrBaseRead, PushForwardDrivesForward) {
  VrBaseComponent base("base_fwd");
  base.configure(base_config());

  VrSession::instance().set_test_frame(make_stick_frame("left", 0.0f, 1.0f));
  const auto out = base.read();
  ASSERT_EQ(out.size(), 2u);
  EXPECT_GT(out[0], 0.0f);              // positive linear = forward
  EXPECT_NEAR(out[0], 0.5f, 1e-4f);     // full deflection = max_linear
  EXPECT_NEAR(out[1], 0.0f, 1e-4f);
}

TEST_F(VrBaseRead, PushBackwardDrivesBackward) {
  VrBaseComponent base("base_back");
  base.configure(base_config());

  VrSession::instance().set_test_frame(make_stick_frame("left", 0.0f, -1.0f));
  EXPECT_LT(base.read()[0], 0.0f);      // negative linear = reverse
}

TEST_F(VrBaseRead, PushLeftTurnsLeft) {
  VrBaseComponent base("base_left");
  base.configure(base_config());

  // Stick left is x_axis < 0; the base must yaw positive (CCW / left).
  VrSession::instance().set_test_frame(make_stick_frame("left", -1.0f, 0.0f));
  const auto out = base.read();
  EXPECT_GT(out[1], 0.0f);
  EXPECT_NEAR(out[1], 1.0f, 1e-4f);     // full deflection = max_angular
}

TEST_F(VrBaseRead, PushRightTurnsRight) {
  VrBaseComponent base("base_right");
  base.configure(base_config());

  VrSession::instance().set_test_frame(make_stick_frame("left", 1.0f, 0.0f));
  EXPECT_LT(base.read()[1], 0.0f);      // negative yaw = CW / right
}

TEST_F(VrBaseRead, WithinDeadzoneOutputsZero) {
  VrBaseComponent base("base_dead");
  base.configure(base_config());

  // Both axes inside the 0.1 deadzone.
  VrSession::instance().set_test_frame(make_stick_frame("left", 0.05f, 0.05f));
  const auto out = base.read();
  EXPECT_NEAR(out[0], 0.0f, 1e-6f);
  EXPECT_NEAR(out[1], 0.0f, 1e-6f);
}

TEST_F(VrBaseRead, CapsAtConfiguredMax) {
  VrBaseComponent base("base_cap");
  base.configure({{"controller_type", "left"},
                  {"max_linear_mps", 0.3},
                  {"max_angular_rps", 0.7},
                  {"deadzone", 0.1}});

  VrSession::instance().set_test_frame(make_stick_frame("left", 0.0f, 1.0f));
  EXPECT_NEAR(base.read()[0], 0.3f, 1e-4f);
}

TEST_F(VrBaseRead, DisconnectedOutputsZero) {
  VrBaseComponent base("base_disc");
  base.configure(base_config());
  VrSession::instance().set_test_frame(make_stick_frame("left", 0.0f, 1.0f));

  VrSession::instance().set_test_connected(false);
  const auto out = base.read();
  EXPECT_NEAR(out[0], 0.0f, 1e-6f);
  EXPECT_NEAR(out[1], 0.0f, 1e-6f);
}

TEST_F(VrBaseRead, NonFiniteAxisStopsBase) {
  VrBaseComponent base("base_nan");
  base.configure(base_config());

  auto bad = make_stick_frame("left", 0.0f, 0.0f);
  bad.left_controller.thumbstick.y_axis = std::nanf("");
  VrSession::instance().set_test_frame(bad);

  const auto out = base.read();
  EXPECT_NEAR(out[0], 0.0f, 1e-6f);
  EXPECT_NEAR(out[1], 0.0f, 1e-6f);
}
