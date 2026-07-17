/**
 * @file test_vr_session.cpp
 * @brief Unit tests for the shared VrSession input-claim system and the
 *        test-frame seam.
 *
 * These exercise the process-wide VR session that every VR hardware component
 * shares. The claim table is the safety mechanism that stops two components
 * fighting over the same controller input, so it is validated directly. No VR
 * hardware is required.
 *
 * VrSession is a singleton, so each test uses unique component ids and releases
 * its claims to avoid leaking state into other tests.
 */

#include <stdexcept>
#include <string>

#include "gtest/gtest.h"

#include "trossen_vr/vr_types.hpp"

#include "trossen_sdk/hw/vr/vr_session.hpp"

using trossen::hw::vr::VrInput;
using trossen::hw::vr::VrSession;
using trossen::hw::vr::VrSessionLease;
using trossen::hw::vr::vr_input_name;

namespace {

VrSession& session() { return VrSession::instance(); }

}  // namespace

// ── claim_inputs: happy paths ───────────────────────────────────────────────

TEST(VrSessionClaims, DisjointInputsCoexist) {
  auto& s = session();
  EXPECT_NO_THROW(s.claim_inputs("right", "arm_a", {VrInput::kPose}));
  EXPECT_NO_THROW(s.claim_inputs("right", "ctrl_a", {VrInput::kButtonOne}));
  s.release_claims("arm_a");
  s.release_claims("ctrl_a");
}

TEST(VrSessionClaims, SameInputDifferentControllerSideIsAllowed) {
  auto& s = session();
  EXPECT_NO_THROW(s.claim_inputs("left", "base_l", {VrInput::kThumbstick}));
  EXPECT_NO_THROW(s.claim_inputs("right", "base_r", {VrInput::kThumbstick}));
  s.release_claims("base_l");
  s.release_claims("base_r");
}

TEST(VrSessionClaims, ReclaimBySameComponentIsIdempotent) {
  auto& s = session();
  s.claim_inputs("right", "arm_idem", {VrInput::kPose, VrInput::kTrigger});
  EXPECT_NO_THROW(
    s.claim_inputs("right", "arm_idem", {VrInput::kPose, VrInput::kTrigger}));
  s.release_claims("arm_idem");
}

// ── claim_inputs: conflicts and validation ──────────────────────────────────

TEST(VrSessionClaims, ConflictingClaimThrows) {
  auto& s = session();
  s.claim_inputs("right", "owner", {VrInput::kPose});
  EXPECT_THROW(s.claim_inputs("right", "intruder", {VrInput::kPose}),
               std::runtime_error);
  s.release_claims("owner");
}

TEST(VrSessionClaims, ConflictLeavesNoPartialClaim) {
  auto& s = session();
  s.claim_inputs("right", "owner2", {VrInput::kTrigger});
  // Requesting {kPose, kTrigger} must fail wholesale on the kTrigger conflict,
  // leaving kPose unclaimed so another component can still take it.
  EXPECT_THROW(
    s.claim_inputs("right", "greedy", {VrInput::kPose, VrInput::kTrigger}),
    std::runtime_error);
  EXPECT_NO_THROW(s.claim_inputs("right", "latecomer", {VrInput::kPose}));
  s.release_claims("owner2");
  s.release_claims("latecomer");
}

TEST(VrSessionClaims, InvalidControllerTypeThrows) {
  EXPECT_THROW(session().claim_inputs("middle", "x", {VrInput::kPose}),
               std::invalid_argument);
}

TEST(VrSessionClaims, EmptyComponentIdThrows) {
  EXPECT_THROW(session().claim_inputs("right", "", {VrInput::kPose}),
               std::invalid_argument);
}

// ── release_claims ──────────────────────────────────────────────────────────

TEST(VrSessionClaims, ReleaseFreesInputForReclaim) {
  auto& s = session();
  s.claim_inputs("right", "first", {VrInput::kPose});
  s.release_claims("first");
  EXPECT_NO_THROW(s.claim_inputs("right", "second", {VrInput::kPose}));
  s.release_claims("second");
}

TEST(VrSessionClaims, ReleaseUnknownComponentIsNoOp) {
  EXPECT_NO_THROW(session().release_claims("never_claimed_anything"));
}

// ── vr_input_name ───────────────────────────────────────────────────────────

TEST(VrSessionInputName, MapsEveryInput) {
  EXPECT_EQ(vr_input_name(VrInput::kPose), "pose");
  EXPECT_EQ(vr_input_name(VrInput::kTrigger), "trigger");
  EXPECT_EQ(vr_input_name(VrInput::kThumbstick), "thumbstick");
  EXPECT_EQ(vr_input_name(VrInput::kButtonOne), "button_one");
  EXPECT_EQ(vr_input_name(VrInput::kButtonTwo), "button_two");
}

// ── test-frame seam ─────────────────────────────────────────────────────────

class VrSessionSeam : public ::testing::Test {
 protected:
  void TearDown() override { session().clear_test_frame(); }
};

TEST_F(VrSessionSeam, InjectedFrameIsReportedConnected) {
  trossen_vr::VRFrame frame;
  frame.right_controller.is_tracked = 1;
  frame.right_controller.pose6d.x   = 0.5;

  session().set_test_frame(frame);
  EXPECT_TRUE(session().is_vr_connected());

  const auto got = session().latest_frame();
  ASSERT_TRUE(got.has_value());
  EXPECT_DOUBLE_EQ(got->right_controller.pose6d.x, 0.5);
}

TEST_F(VrSessionSeam, SetDisconnectedOverridesConnectionFlag) {
  trossen_vr::VRFrame frame;
  session().set_test_frame(frame);
  ASSERT_TRUE(session().is_vr_connected());

  session().set_test_connected(false);
  EXPECT_FALSE(session().is_vr_connected());
}

TEST_F(VrSessionSeam, ClearLeavesTestMode) {
  session().set_test_frame(trossen_vr::VRFrame{});
  session().clear_test_frame();
  EXPECT_FALSE(session().is_vr_connected());
  EXPECT_FALSE(session().latest_frame().has_value());
}

// ── VrSessionLease: RAII reference + claim cleanup ───────────────────────────

TEST_F(VrSessionSeam, LeaseReleasesClaimsOnDestruction) {
  session().set_test_frame(trossen_vr::VRFrame{});
  // Keep-alive lease so the reference count never reaches zero during this test.
  // That way the claim cleanup below can only come from the destroyed lease's
  // own release_claims(), not from the full-teardown clear.
  VrSessionLease keepalive;
  keepalive.acquire(9000, "keepalive");
  {
    VrSessionLease lease;
    lease.acquire(9000, "lease_owner");
    session().claim_inputs("right", "lease_owner", {VrInput::kPose});
    EXPECT_THROW(session().claim_inputs("right", "intruder", {VrInput::kPose}),
                 std::runtime_error);
  }  // lease goes out of scope: releases its reference and its own claims.
  EXPECT_NO_THROW(session().claim_inputs("right", "intruder", {VrInput::kPose}));
  session().release_claims("intruder");
  keepalive.reset();
}

TEST_F(VrSessionSeam, LeaseResetIsIdempotent) {
  session().set_test_frame(trossen_vr::VRFrame{});
  VrSessionLease lease;
  lease.acquire(9000, "idem_owner");
  EXPECT_TRUE(lease.held());
  lease.reset();
  EXPECT_FALSE(lease.held());
  // A second release must be a no-op, so a lease can never over-release and drop
  // the shared connection out from under another holder.
  EXPECT_NO_THROW(lease.reset());
  EXPECT_FALSE(lease.held());
}

TEST_F(VrSessionSeam, LeaseMoveTransfersOwnership) {
  session().set_test_frame(trossen_vr::VRFrame{});
  VrSessionLease a;
  a.acquire(9000, "move_owner");
  EXPECT_TRUE(a.held());
  VrSessionLease b = std::move(a);
  EXPECT_FALSE(a.held());  // moved-from lease no longer owns the reference
  EXPECT_TRUE(b.held());
}
