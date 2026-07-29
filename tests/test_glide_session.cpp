/**
 * @file test_glide_session.cpp
 * @brief Tests for Glide input arbitration, claim leases, and the reader seam.
 */

#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "trossen_sdk/hw/glide/glide_session.hpp"

namespace {

using trossen::hw::glide::GlideClaim;
using trossen::hw::glide::GlideClaimLease;
using trossen::hw::glide::GlideInput;
using trossen::hw::glide::GlideInputSnapshot;
using trossen::hw::glide::GlideSession;
using trossen::hw::glide::glide_button;

/// GlideSession is a process-global singleton, so every case starts from a
/// clean table — otherwise one case's claims fail an unrelated later case.
class GlideSessionTest : public ::testing::Test {
protected:
  void SetUp() override { GlideSession::instance().reset_for_test(); }
  void TearDown() override { GlideSession::instance().reset_for_test(); }

  GlideSession& session() { return GlideSession::instance(); }
};

// ── Snapshot bit math ────────────────────────────────────────────────────

TEST_F(GlideSessionTest, SnapshotReadsSetBits) {
  GlideInputSnapshot snapshot;
  snapshot.buttons = 0b1010;

  EXPECT_FALSE(snapshot.button(0));
  EXPECT_TRUE(snapshot.button(1));
  EXPECT_FALSE(snapshot.button(2));
  EXPECT_TRUE(snapshot.button(3));
}

TEST_F(GlideSessionTest, SnapshotOutOfRangeBitsReadFalse) {
  GlideInputSnapshot snapshot;
  snapshot.buttons = 0xFFFFFFFFu;

  // Guards against a negative or oversized bit index shifting out of bounds,
  // which would be undefined behavior rather than a clean false.
  EXPECT_FALSE(snapshot.button(-1));
  EXPECT_FALSE(snapshot.button(32));
  EXPECT_FALSE(snapshot.button(99));
  EXPECT_TRUE(snapshot.button(31));
}

// ── Claim arbitration ────────────────────────────────────────────────────

TEST_F(GlideSessionTest, ClaimThenHolderReportsOwner) {
  session().claim_inputs("glide_left", "base_leader", {GlideClaim{GlideInput::kJoystick}});

  const auto holder =
    session().claim_holder("glide_left", GlideClaim{GlideInput::kJoystick});
  ASSERT_TRUE(holder.has_value());
  EXPECT_EQ(*holder, "base_leader");
}

TEST_F(GlideSessionTest, UnclaimedInputHasNoHolder) {
  EXPECT_FALSE(
    session().claim_holder("glide_left", GlideClaim{GlideInput::kJoystick}).has_value());
}

TEST_F(GlideSessionTest, ConflictingClaimFromOtherComponentThrows) {
  session().claim_inputs("glide_right", "base_leader", {glide_button(3)});

  EXPECT_THROW(
    session().claim_inputs("glide_right", "session_control", {glide_button(3)}),
    std::runtime_error);
}

TEST_F(GlideSessionTest, SameButtonOnDifferentHandlesDoesNotConflict) {
  // Bit 3 on the left handle and bit 3 on the right handle are different
  // physical buttons; keying claims by handle is what makes that work.
  session().claim_inputs("glide_left", "session_control", {glide_button(3)});
  EXPECT_NO_THROW(
    session().claim_inputs("glide_right", "base_leader", {glide_button(3)}));
}

TEST_F(GlideSessionTest, RepeatedClaimBySameComponentIsIdempotent) {
  session().claim_inputs("glide_left", "base_leader", {GlideClaim{GlideInput::kJoystick}});
  EXPECT_NO_THROW(
    session().claim_inputs("glide_left", "base_leader", {GlideClaim{GlideInput::kJoystick}}));
}

TEST_F(GlideSessionTest, RejectedClaimSetLeavesTableUnchanged) {
  session().claim_inputs("glide_left", "first", {glide_button(2)});

  // Second component asks for a free button and a taken one in one call. The
  // whole set must be rejected — a half-applied claim would leave it holding
  // bit 5 it never learned it owned.
  EXPECT_THROW(
    session().claim_inputs("glide_left", "second", {glide_button(5), glide_button(2)}),
    std::runtime_error);

  EXPECT_FALSE(session().claim_holder("glide_left", glide_button(5)).has_value());
}

TEST_F(GlideSessionTest, ReleaseClaimsFreesOnlyThatComponent) {
  session().claim_inputs("glide_left", "base_leader", {glide_button(1)});
  session().claim_inputs("glide_left", "session_control", {glide_button(2)});

  session().release_claims("base_leader");

  EXPECT_FALSE(session().claim_holder("glide_left", glide_button(1)).has_value());
  EXPECT_TRUE(session().claim_holder("glide_left", glide_button(2)).has_value());
}

TEST_F(GlideSessionTest, ReleaseWithNoClaimsIsSafe) {
  EXPECT_NO_THROW(session().release_claims("never_claimed_anything"));
}

TEST_F(GlideSessionTest, EmptyIdsAreRejected) {
  EXPECT_THROW(
    session().claim_inputs("", "base_leader", {glide_button(1)}),
    std::invalid_argument);
  EXPECT_THROW(
    session().claim_inputs("glide_left", "", {glide_button(1)}),
    std::invalid_argument);
}

TEST_F(GlideSessionTest, OutOfRangeButtonBitIsRejected) {
  EXPECT_THROW(
    session().claim_inputs("glide_left", "base_leader", {glide_button(-1)}),
    std::invalid_argument);
  EXPECT_THROW(
    session().claim_inputs("glide_left", "base_leader", {glide_button(32)}),
    std::invalid_argument);
}

// ── Claim lease ──────────────────────────────────────────────────────────

TEST_F(GlideSessionTest, LeaseReleasesOnDestruction) {
  {
    GlideClaimLease lease;
    lease.add("glide_left", "base_leader", {GlideClaim{GlideInput::kJoystick}});
    EXPECT_TRUE(lease.held());
    EXPECT_TRUE(
      session().claim_holder("glide_left", GlideClaim{GlideInput::kJoystick}).has_value());
  }

  EXPECT_FALSE(
    session().claim_holder("glide_left", GlideClaim{GlideInput::kJoystick}).has_value());
}

TEST_F(GlideSessionTest, LeaseAccumulatesClaimsAcrossHandles) {
  GlideClaimLease lease;
  lease.add("glide_left", "base_leader", {GlideClaim{GlideInput::kJoystick}});
  lease.add("glide_right", "base_leader", {GlideClaim{GlideInput::kJoystick}});

  EXPECT_TRUE(
    session().claim_holder("glide_left", GlideClaim{GlideInput::kJoystick}).has_value());
  EXPECT_TRUE(
    session().claim_holder("glide_right", GlideClaim{GlideInput::kJoystick}).has_value());

  lease.reset();

  // One release covers every handle, because releases are component-wide.
  EXPECT_FALSE(
    session().claim_holder("glide_left", GlideClaim{GlideInput::kJoystick}).has_value());
  EXPECT_FALSE(
    session().claim_holder("glide_right", GlideClaim{GlideInput::kJoystick}).has_value());
}

TEST_F(GlideSessionTest, LeaseRejectsSecondComponentId) {
  GlideClaimLease lease;
  lease.add("glide_left", "base_leader", {glide_button(1)});

  EXPECT_THROW(
    lease.add("glide_left", "other_component", {glide_button(2)}),
    std::invalid_argument);
}

TEST_F(GlideSessionTest, LeaseResetIsIdempotent) {
  GlideClaimLease lease;
  lease.add("glide_left", "base_leader", {glide_button(1)});
  lease.reset();
  EXPECT_NO_THROW(lease.reset());
  EXPECT_FALSE(lease.held());
}

TEST_F(GlideSessionTest, FailedClaimLeavesLeaseUnheld) {
  session().claim_inputs("glide_left", "incumbent", {glide_button(1)});

  GlideClaimLease lease;
  EXPECT_THROW(lease.add("glide_left", "latecomer", {glide_button(1)}),
               std::runtime_error);

  // The lease must not think it owns anything, or its destructor would release
  // the incumbent's claim.
  EXPECT_FALSE(lease.held());
  lease.reset();
  EXPECT_EQ(*session().claim_holder("glide_left", glide_button(1)), "incumbent");
}

// ── Reader dispatch ──────────────────────────────────────────────────────

TEST_F(GlideSessionTest, ReadWithNoReaderReturnsNullopt) {
  EXPECT_FALSE(session().read_inputs("glide_left").has_value());
}

TEST_F(GlideSessionTest, RegisteredReaderIsDispatched) {
  GlideInputSnapshot snapshot;
  snapshot.joystick_x = 1234;
  snapshot.buttons    = 0b100;

  session().register_reader("glide_left", [snapshot]() { return snapshot; });

  const auto read = session().read_inputs("glide_left");
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->joystick_x, 1234);
  EXPECT_TRUE(read->button(2));
}

TEST_F(GlideSessionTest, ReaderReportingNoDataPropagatesNullopt) {
  session().register_reader(
    "glide_left", []() { return std::optional<GlideInputSnapshot>{}; });
  EXPECT_FALSE(session().read_inputs("glide_left").has_value());
}

TEST_F(GlideSessionTest, UnregisterReaderStopsDispatch) {
  session().register_reader("glide_left", []() { return GlideInputSnapshot{}; });
  ASSERT_TRUE(session().read_inputs("glide_left").has_value());

  session().unregister_reader("glide_left");
  EXPECT_FALSE(session().read_inputs("glide_left").has_value());
}

TEST_F(GlideSessionTest, TestSnapshotOverridesRegisteredReader) {
  GlideInputSnapshot from_hardware;
  from_hardware.joystick_x = 100;
  session().register_reader("glide_left", [from_hardware]() { return from_hardware; });

  GlideInputSnapshot injected;
  injected.joystick_x = 4095;
  session().set_test_snapshot("glide_left", injected);

  ASSERT_TRUE(session().read_inputs("glide_left").has_value());
  EXPECT_EQ(session().read_inputs("glide_left")->joystick_x, 4095);

  session().clear_test_snapshots();
  EXPECT_EQ(session().read_inputs("glide_left")->joystick_x, 100);
}

}  // namespace
