/**
 * @file test_vr_arm_component.cpp
 * @brief Unit tests for the VR arm Cartesian teleop leader.
 *
 * Drives the real VrArmComponent with synthetic frames via the VrSession
 * test seam — no VR hardware required. Covers config validation, the
 * pose offset math, the trigger-to-gripper mapping, deadman re-grip
 * (resume-in-place), and the non-finite pose guard.
 */

#include <cmath>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_vr/vr_types.hpp"

#include "trossen_sdk/hw/vr/vr_arm_component.hpp"
#include "trossen_sdk/hw/vr/vr_session.hpp"

using trossen::hw::vr::VrArmComponent;
using trossen::hw::vr::VrSession;

namespace {

// Build a single-controller frame with a zero-rotation pose so position maps
// linearly through the transform.
trossen_vr::VRFrame make_frame(const std::string& side, double x, double y,
                               double z, double trigger, bool tracked = true) {
  trossen_vr::VRFrame f;
  auto& c = (side == "right") ? f.right_controller : f.left_controller;
  c.is_tracked = tracked ? 1 : 0;
  c.pose6d.x = x;
  c.pose6d.y = y;
  c.pose6d.z = z;
  c.triggers.index_trigger = trigger;
  return f;
}

nlohmann::json arm_config() {
  return nlohmann::json{
    {"controller_type", "right"},
    {"gripper_min_m", 0.0},
    {"gripper_max_m", 0.04},
  };
}

}  // namespace

// ── config validation (no frames needed) ────────────────────────────────────

TEST(VrArmConfig, MissingControllerTypeThrows) {
  VrArmComponent arm("arm_cfg1");
  EXPECT_THROW(arm.configure(nlohmann::json::object()), std::runtime_error);
}

TEST(VrArmConfig, InvalidControllerTypeThrows) {
  VrArmComponent arm("arm_cfg2");
  EXPECT_THROW(arm.configure({{"controller_type", "middle"}}),
               std::runtime_error);
}

TEST(VrArmConfig, GripperMaxBelowMinThrows) {
  VrArmComponent arm("arm_cfg3");
  EXPECT_THROW(
    arm.configure({{"controller_type", "right"},
                   {"gripper_min_m", 0.05},
                   {"gripper_max_m", 0.01}}),
    std::runtime_error);
}

TEST(VrArmConfig, NegativeConnectionTimeoutThrows) {
  VrArmComponent arm("arm_cfg4");
  EXPECT_THROW(
    arm.configure({{"controller_type", "right"},
                   {"connection_timeout_s", -1.0}}),
    std::runtime_error);
}

// ── transform + gripper behavior (seam-driven) ──────────────────────────────

class VrArmRead : public ::testing::Test {
 protected:
  // Enter test mode before configuring so no real socket is opened.
  void SetUp() override {
    VrSession::instance().set_test_frame(trossen_vr::VRFrame{});
  }
  void TearDown() override { VrSession::instance().clear_test_frame(); }
};

TEST_F(VrArmRead, ResumesInPlaceOnSync) {
  // With the controller tracked at sync time, the first read reproduces the
  // follower's current pose rather than snapping to the controller's position.
  VrSession::instance().set_test_frame(make_frame("right", 0.9, -0.3, 0.5, 0.5));

  VrArmComponent arm("arm_resume");
  arm.configure(arm_config());
  arm.sync_to_state({0.4f, 0.1f, 0.2f, 0.0f, 0.0f, 0.0f, 0.02f});

  const auto out = arm.read();
  ASSERT_EQ(out.size(), 7u);
  EXPECT_NEAR(out[0], 0.4f, 1e-4f);
  EXPECT_NEAR(out[1], 0.1f, 1e-4f);
  EXPECT_NEAR(out[2], 0.2f, 1e-4f);
  EXPECT_NEAR(out[6], 0.02f, 1e-4f);  // trigger 0.5 → mid gripper
}

TEST_F(VrArmRead, ControllerTranslationMovesTargetByTheSameDelta) {
  VrSession::instance().set_test_frame(make_frame("right", 0.9, -0.3, 0.5, 0.0));

  VrArmComponent arm("arm_move");
  arm.configure(arm_config());
  arm.sync_to_state({0.4f, 0.1f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f});

  // Move the controller +0.1 m in x; the target must move +0.1 m in x.
  VrSession::instance().set_test_frame(make_frame("right", 1.0, -0.3, 0.5, 0.0));
  const auto out = arm.read();
  ASSERT_EQ(out.size(), 7u);
  EXPECT_NEAR(out[0], 0.5f, 1e-4f);
  EXPECT_NEAR(out[1], 0.1f, 1e-4f);
  EXPECT_NEAR(out[2], 0.2f, 1e-4f);
}

TEST_F(VrArmRead, TriggerMapsToGripperRange) {
  VrArmComponent arm("arm_grip");
  VrSession::instance().set_test_frame(make_frame("right", 0.0, 0.0, 0.0, 0.0));
  arm.configure(arm_config());
  arm.sync_to_state({0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});

  VrSession::instance().set_test_frame(make_frame("right", 0.0, 0.0, 0.0, 1.0));
  EXPECT_NEAR(arm.read()[6], 0.04f, 1e-4f);

  VrSession::instance().set_test_frame(make_frame("right", 0.0, 0.0, 0.0, 0.0));
  EXPECT_NEAR(arm.read()[6], 0.0f, 1e-4f);
}

TEST_F(VrArmRead, ReGripResumesInPlaceAtNewControllerPose) {
  VrSession::instance().set_test_frame(make_frame("right", 0.9, -0.3, 0.5, 0.0));
  VrArmComponent arm("arm_regrip");
  arm.configure(arm_config());
  arm.sync_to_state({0.4f, 0.1f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f});
  const auto anchored = arm.read();  // target ≈ {0.4, 0.1, 0.2}

  // Release the deadman (untracked): the target must hold.
  VrSession::instance().set_test_frame(
    make_frame("right", 0.9, -0.3, 0.5, 0.0, /*tracked=*/false));
  const auto held = arm.read();
  EXPECT_NEAR(held[0], anchored[0], 1e-4f);

  // Re-engage far away: the target must resume in place, not jump.
  VrSession::instance().set_test_frame(make_frame("right", 3.0, 2.0, -1.0, 0.0));
  const auto resumed = arm.read();
  EXPECT_NEAR(resumed[0], anchored[0], 1e-4f);
  EXPECT_NEAR(resumed[1], anchored[1], 1e-4f);
  EXPECT_NEAR(resumed[2], anchored[2], 1e-4f);
}

TEST_F(VrArmRead, NonFinitePoseHoldsLastGood) {
  VrSession::instance().set_test_frame(make_frame("right", 0.9, -0.3, 0.5, 0.0));
  VrArmComponent arm("arm_nan");
  arm.configure(arm_config());
  arm.sync_to_state({0.4f, 0.1f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f});
  const auto good = arm.read();

  // Poison the pose; read() must reject it and return the last good target.
  auto bad = make_frame("right", 0.9, -0.3, 0.5, 0.0);
  bad.right_controller.pose6d.x = std::nan("");
  VrSession::instance().set_test_frame(bad);

  const auto out = arm.read();
  ASSERT_EQ(out.size(), good.size());
  EXPECT_NEAR(out[0], good[0], 1e-4f);
  EXPECT_NEAR(out[1], good[1], 1e-4f);
  EXPECT_NEAR(out[2], good[2], 1e-4f);
}
