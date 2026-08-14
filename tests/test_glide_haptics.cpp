/**
 * @file test_glide_haptics.cpp
 * @brief Unit tests for the force-to-vibration curve and the resting-level
 *        tracker that decides what counts as contact in the first place.
 */

#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "trossen_sdk/hw/glide/glide_haptics.hpp"

namespace {

using trossen::hw::glide::GlideHapticCurve;
using trossen::hw::glide::HapticBaseline;
using trossen::hw::glide::command_clears_zero;

/// A curve with round numbers, so the expected values below are obvious by
/// inspection rather than the output of the code under test.
GlideHapticCurve curve() {
  GlideHapticCurve c;
  c.deadband_n      = 5.0f;
  c.max_n           = 45.0f;   // span of 40 N
  c.intensity_floor = 100;
  c.intensity_max   = 200;     // duty range of 100
  c.gamma           = 1.0f;
  c.levels          = 0;       // quantisation off unless a test asks for it
  return c;
}

// ── The dead zone ────────────────────────────────────────────────────────

TEST(GlideHapticCurve, NoForceIsSilent) {
  EXPECT_EQ(curve().intensity_for_force(0.0f), 0);
}

TEST(GlideHapticCurve, ForceInsideTheDeadZoneIsSilent) {
  // The residual on a free-moving arm lives here. If this rendered, the handle
  // would buzz whenever the operator moved quickly and mean nothing.
  EXPECT_EQ(curve().intensity_for_force(4.9f), 0);
  EXPECT_EQ(curve().intensity_for_force(5.0f), 0);
}

TEST(GlideHapticCurve, FirstForcePastTheDeadZoneJumpsToTheFloor) {
  // Not a ramp from zero: below its start-up duty the motor does not spin, so
  // anything less than the floor would be felt as continued silence.
  EXPECT_EQ(curve().intensity_for_force(5.01f), 100);
}

// ── The linear range and saturation ──────────────────────────────────────

TEST(GlideHapticCurve, MidRangeForceIsHalfwayUpTheDutyRange) {
  // 25 N is halfway across a 5-45 N span, so halfway across a 100-200 duty range.
  EXPECT_EQ(curve().intensity_for_force(25.0f), 150);
}

TEST(GlideHapticCurve, SaturationForceGivesMaxIntensity) {
  EXPECT_EQ(curve().intensity_for_force(45.0f), 200);
}

TEST(GlideHapticCurve, ForceBeyondSaturationClampsRatherThanOverflowing) {
  // 0-255 is a uint8_t: an unclamped linear extrapolation here would wrap
  // around and render a hard push as a faint tickle.
  EXPECT_EQ(curve().intensity_for_force(400.0f), 200);
  EXPECT_EQ(curve().intensity_for_force(1.0e9f), 200);
}

TEST(GlideHapticCurve, IntensityRisesMonotonicallyWithForce) {
  const auto c = curve();
  int previous = -1;
  for (float f = 0.0f; f <= 60.0f; f += 0.25f) {
    const int now = c.intensity_for_force(f);
    EXPECT_GE(now, previous) << "intensity fell at " << f << " N";
    previous = now;
  }
}

// ── Sign and non-finite input ────────────────────────────────────────────

TEST(GlideHapticCurve, NegativeForceIsTreatedAsMagnitude) {
  // Callers pass a vector magnitude, but a sign convention change upstream must
  // not silently invert the curve into permanent silence.
  EXPECT_EQ(curve().intensity_for_force(-25.0f), 150);
}

TEST(GlideHapticCurve, NonFiniteForceIsSilentRatherThanUndefined) {
  // Reached when a driver read races a reconnect. The motor latches, so the
  // only safe answer to "no idea" is zero.
  const auto c = curve();
  EXPECT_EQ(c.intensity_for_force(std::numeric_limits<float>::quiet_NaN()), 0);
  EXPECT_EQ(c.intensity_for_force(std::numeric_limits<float>::infinity()), 0);
  EXPECT_EQ(c.intensity_for_force(-std::numeric_limits<float>::infinity()), 0);
}

// ── Shaping ──────────────────────────────────────────────────────────────

TEST(GlideHapticCurve, GammaAboveOneDelaysTheOnset) {
  GlideHapticCurve c = curve();
  c.gamma = 2.0f;
  // Halfway along the force span is now a quarter of the way up the duty range.
  EXPECT_EQ(c.intensity_for_force(25.0f), 125);
  // The ends are fixed points of any gamma.
  EXPECT_EQ(c.intensity_for_force(5.01f), 100);
  EXPECT_EQ(c.intensity_for_force(45.0f), 200);
}

TEST(GlideHapticCurve, GammaBelowOneSharpensLightContact) {
  GlideHapticCurve c = curve();
  c.gamma = 0.5f;
  EXPECT_EQ(c.intensity_for_force(25.0f), 171);  // 100 + sqrt(0.5)*100
}

// ── Quantisation, which is what protects the handle's link ───────────────

TEST(GlideHapticCurve, QuantisationCollapsesTheRangeToNSteps) {
  GlideHapticCurve c = curve();
  c.levels = 5;  // 5 buckets across the duty range: 100/125/150/175/200

  std::set<int> seen;
  for (float f = 5.01f; f <= 45.0f; f += 0.1f) {
    seen.insert(c.intensity_for_force(f));
  }
  EXPECT_EQ(seen, (std::set<int>{100, 125, 150, 175, 200}));
}

TEST(GlideHapticCurve, QuantisationIsWhatMakesASteadyLeanCostOnePacket) {
  GlideHapticCurve c = curve();
  c.levels = 8;
  // Two forces 1 N apart mid-range land in the same bucket, so GlideSession's
  // change detection swallows the second write. Without quantisation, noise of
  // this size would push a packet every interval.
  EXPECT_EQ(c.intensity_for_force(25.0f), c.intensity_for_force(26.0f));
}

TEST(GlideHapticCurve, QuantisationBelowTwoLevelsIsDisabled) {
  GlideHapticCurve c = curve();
  c.levels = 1;
  // Documented behaviour: too few levels to bucket anything means don't try.
  // (Dividing by levels - 1 here would be a divide by zero.)
  EXPECT_EQ(c.intensity_for_force(25.0f), 150);
  c.levels = 0;
  EXPECT_EQ(c.intensity_for_force(25.0f), 150);
}

// ── Validation ───────────────────────────────────────────────────────────

TEST(GlideHapticCurve, ValidCurvePasses) {
  EXPECT_NO_THROW(curve().validate("arm"));
}

TEST(GlideHapticCurve, SaturationBelowDeadZoneIsRejected) {
  GlideHapticCurve c = curve();
  c.max_n = 1.0f;
  EXPECT_THROW(c.validate("arm"), std::invalid_argument);
}

TEST(GlideHapticCurve, ZeroSpanIsRejected) {
  // Would divide by zero in the mapping, so it has to fail at configure() time.
  GlideHapticCurve c = curve();
  c.max_n = c.deadband_n;
  EXPECT_THROW(c.validate("arm"), std::invalid_argument);
}

TEST(GlideHapticCurve, NegativeDeadZoneIsRejected) {
  GlideHapticCurve c = curve();
  c.deadband_n = -1.0f;
  EXPECT_THROW(c.validate("arm"), std::invalid_argument);
}

TEST(GlideHapticCurve, InvertedIntensityRangeIsRejected) {
  GlideHapticCurve c = curve();
  c.intensity_floor = 200;
  c.intensity_max   = 100;
  EXPECT_THROW(c.validate("arm"), std::invalid_argument);
}

TEST(GlideHapticCurve, NonPositiveGammaIsRejected) {
  GlideHapticCurve c = curve();
  c.gamma = 0.0f;
  EXPECT_THROW(c.validate("arm"), std::invalid_argument);
  c.gamma = -1.0f;
  EXPECT_THROW(c.validate("arm"), std::invalid_argument);
}

TEST(GlideHapticCurve, ValidationMessageNamesTheOffendingArm) {
  GlideHapticCurve c = curve();
  c.max_n = 1.0f;
  try {
    c.validate("glide_left");
    FAIL() << "expected validate() to throw";
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(std::string(e.what()).find("glide_left"), std::string::npos);
  }
}

// ── HapticBaseline ────────────────────────────────────────────────────────
//
// Measured on hardware: an idle Rivet follower reports ~50 N of external-effort
// residual with nothing touching it, so these tests use a resting level in that
// range rather than a tidy zero. Rendering that raw number is what pinned the
// motor at full duty, and it is the failure these tests exist to prevent.

namespace {
constexpr float kRest = 50.0f;      // measured idle residual, N
constexpr float kDead = 5.0f;       // dead zone, N
constexpr float kTau  = 3.0f;       // tracking time constant, s

/// Settles the tracker at `rest` by feeding it that level for `seconds`.
HapticBaseline settled(float rest, double& t, double seconds = 10.0,
                       float tau = kTau, float dead = kDead) {
  HapticBaseline b(tau, dead);
  const double step = 0.01;
  for (double elapsed = 0.0; elapsed < seconds; elapsed += step) {
    t += step;
    b.deviation_for(rest, t);
  }
  return b;
}
}  // namespace

TEST(HapticBaseline, TheFirstReadingIsSilentAndBecomesTheRestingLevel) {
  HapticBaseline b(kTau, kDead);
  EXPECT_FALSE(b.measured());

  // Silent even though 50 N would saturate the curve outright: the first sample
  // defines "at rest", so a session starts quiet instead of buzzing until the
  // tracker settles.
  EXPECT_FLOAT_EQ(b.deviation_for(kRest, 1.0), 0.0f);
  EXPECT_TRUE(b.measured());
  EXPECT_NEAR(b.level(), kRest, 1e-3);
}

TEST(HapticBaseline, AnUntouchedArmStaysSilentAtAnyRestingLevel) {
  // The regression test for the reported bug: the handle buzzed constantly with
  // nothing touching the arm.
  double t = 0.0;
  HapticBaseline b = settled(kRest, t);

  for (int i = 0; i < 200; ++i) {
    t += 0.01;
    EXPECT_FLOAT_EQ(b.deviation_for(kRest, t), 0.0f);
  }
}

TEST(HapticBaseline, RestingNoiseBelowTheDeadZoneStaysSilent) {
  // The measured residual wanders a few N around its mean, which must not read
  // as contact.
  double t = 0.0;
  HapticBaseline b = settled(kRest, t);

  const float wobble[] = {-3.0f, 2.5f, -1.0f, 3.5f, -2.0f, 1.5f};
  for (int pass = 0; pass < 20; ++pass) {
    for (float w : wobble) {
      t += 0.01;
      EXPECT_LE(b.deviation_for(kRest + w, t), kDead)
        << "resting noise must not exceed the dead zone";
    }
  }
}

TEST(HapticBaseline, ContactIsReportedAsDeviationNotAbsoluteForce) {
  double t = 0.0;
  HapticBaseline b = settled(kRest, t);

  t += 0.01;
  // 20 N of contact on top of a 50 N resting level must read as 20, not 70 --
  // otherwise every contact saturates a curve whose max is 40.
  EXPECT_NEAR(b.deviation_for(kRest + 20.0f, t), 20.0f, 0.5f);
}

TEST(HapticBaseline, ASustainedPushDoesNotFadeOut) {
  // The reason adaptation freezes above the dead zone. A plain low-pass would
  // learn the push and go quiet under the operator's hand while they were still
  // leaning on something.
  double t = 0.0;
  HapticBaseline b = settled(kRest, t);

  float last = 0.0f;
  for (double elapsed = 0.0; elapsed < 30.0; elapsed += 0.01) {
    t += 0.01;
    last = b.deviation_for(kRest + 20.0f, t);
  }
  EXPECT_NEAR(last, 20.0f, 0.5f)
    << "a 30s push (10x the time constant) must still be felt";
}

TEST(HapticBaseline, ReleasingAContactReturnsToSilence) {
  double t = 0.0;
  HapticBaseline b = settled(kRest, t);

  for (double elapsed = 0.0; elapsed < 5.0; elapsed += 0.01) {
    t += 0.01;
    b.deviation_for(kRest + 20.0f, t);
  }
  t += 0.01;
  EXPECT_FLOAT_EQ(b.deviation_for(kRest, t), 0.0f);
}

TEST(HapticBaseline, AnArmThatStartsInContactRecoversInsteadOfStayingNumb) {
  // The known cost of measuring the level from the arm's own state: a contact
  // present at the first reading is taken for the resting level and cannot be
  // felt. It must not be permanent -- once released, the level adapts DOWN
  // (negative deviations never freeze) and the arm becomes sensitive again.
  double t = 1.0;
  HapticBaseline b(kTau, kDead);
  b.deviation_for(kRest + 20.0f, t);   // starts pressed
  EXPECT_NEAR(b.level(), kRest + 20.0f, 1e-3);

  for (double elapsed = 0.0; elapsed < 20.0; elapsed += 0.01) {
    t += 0.01;
    b.deviation_for(kRest, t);          // released
  }
  EXPECT_NEAR(b.level(), kRest, 0.5f);

  t += 0.01;
  EXPECT_NEAR(b.deviation_for(kRest + 20.0f, t), 20.0f, 0.5f);
}

TEST(HapticBaseline, ThePoseResidualIsTrackedSoItDoesNotReadAsContact) {
  // The reason this is tracked rather than configured as a constant: moving the
  // arm changes the residual, and a fixed offset measured in one pose would make
  // every other pose buzz. Ramped in slowly, as a pose change is.
  double t = 0.0;
  HapticBaseline b = settled(kRest, t);

  // +15 N over 30 s = 0.5 N/s, well inside the dead zone per step, so it is
  // learned rather than latched as contact.
  constexpr float kRate = 0.5f;   // N/s
  for (int i = 1; i <= 3000; ++i) {
    t += 0.01;
    b.deviation_for(kRest + kRate * 0.01f * static_cast<float>(i), t);
  }

  // An exponential tracker follows a ramp with a steady-state lag of exactly
  // rate * tau, so it settles 1.5 N behind rather than dead on. Asserted rather
  // than tolerated: that lag is the price of the time constant, and it is also
  // the budget the dead zone has to cover, so a change in either should fail here
  // rather than quietly start buzzing on every pose change.
  EXPECT_NEAR(b.level(), kRest + 15.0f - kRate * kTau, 0.2f);

  t += 0.01;
  EXPECT_LE(b.deviation_for(kRest + 15.0f, t), kDead)
    << "the new pose must be the new normal, not a permanent buzz";
}

TEST(HapticBaseline, ZeroTauNeverAdaptsAndRendersTheRawResidual) {
  // The characterisation mode, and a guard on the meaning of 0: it must disable
  // tracking rather than divide by zero or adapt instantly.
  double t = 1.0;
  HapticBaseline b(0.0f, kDead);
  b.deviation_for(0.0f, t);            // level = 0

  t += 1.0;
  EXPECT_NEAR(b.deviation_for(kRest, t), kRest, 1e-3);
  t += 100.0;
  EXPECT_NEAR(b.deviation_for(kRest, t), kRest, 1e-3) << "must never adapt";
}

TEST(HapticBaseline, ResetForgetsTheLevelSoTheNextSessionRemeasures) {
  double t = 0.0;
  HapticBaseline b = settled(kRest, t);
  ASSERT_TRUE(b.measured());

  b.reset();
  EXPECT_FALSE(b.measured());

  // A level learned with the arm limp must not carry into a session that starts
  // in a different pose -- the next reading defines the level afresh, silently.
  t += 0.01;
  EXPECT_FLOAT_EQ(b.deviation_for(12.0f, t), 0.0f);
  EXPECT_NEAR(b.level(), 12.0f, 1e-3);
}

TEST(HapticBaseline, ANonFiniteReadingIsDroppedAndDoesNotPoisonTheLevel) {
  // A NaN folded into the level would never wash out: every later comparison
  // against it is false, so the handle would go silent for the rest of the
  // session. Reached in practice when a driver read races a reconnect.
  double t = 0.0;
  HapticBaseline b = settled(kRest, t);

  t += 0.01;
  EXPECT_FLOAT_EQ(b.deviation_for(std::nanf(""), t), 0.0f);
  t += 0.01;
  EXPECT_FLOAT_EQ(
    b.deviation_for(std::numeric_limits<float>::infinity(), t), 0.0f);

  EXPECT_NEAR(b.level(), kRest, 0.5f) << "the level must survive intact";
  t += 0.01;
  EXPECT_NEAR(b.deviation_for(kRest + 20.0f, t), 20.0f, 0.5f);
}

// ── command_clears_zero: the "is the arm parked" gate ─────────────────────
//
// Measured on rivet-02: a parked follower sits at essentially zero
// configuration (-0.006, 0.000, 0.003, -0.004, -0.003, -0.003, 0.000) while
// reporting ~50 N of residual, which is the case this gate suppresses.

TEST(CommandClearsZero, TheParkedPoseIsGated) {
  const std::vector<float> parked =
    {-0.006f, 0.000f, 0.003f, -0.004f, -0.003f, -0.003f, 0.000f};
  EXPECT_FALSE(command_clears_zero(parked, 0.002f));
}

TEST(CommandClearsZero, AWorkingPoseOpensTheGate) {
  const std::vector<float> working = {0.4f, -0.25f, 0.31f, -0.12f, 0.08f, 0.5f, 0.03f};
  EXPECT_TRUE(command_clears_zero(working, 0.002f));
}

TEST(CommandClearsZero, AnEmptyCommandIsGated) {
  // The mirror treats an empty leader read as "nothing moved", so it must not be
  // read here as a command of zeros that happens to be vacuously non-zero.
  EXPECT_FALSE(command_clears_zero({}, 0.002f));
}

TEST(CommandClearsZero, AClosedGripperGatesTheWholeArm) {
  // ACCEPTED COST of the all-elements rule, pinned here so it is a known
  // behaviour rather than a surprise in the hand: a closed gripper commands ~0,
  // so contact is not felt while grasping. Relaxing to "any element" is the fix
  // if this turns out to matter.
  const std::vector<float> grasping = {0.4f, -0.25f, 0.31f, -0.12f, 0.08f, 0.5f, 0.0f};
  EXPECT_FALSE(command_clears_zero(grasping, 0.002f));
}

TEST(CommandClearsZero, OneJointCrossingZeroGatesTheWholeArm) {
  // The other accepted cost: the buzz flickers as a joint passes through zero.
  std::vector<float> moving = {0.4f, -0.25f, 0.31f, -0.12f, 0.08f, 0.5f, 0.03f};
  ASSERT_TRUE(command_clears_zero(moving, 0.002f));
  moving[3] = 0.001f;   // mid-crossing
  EXPECT_FALSE(command_clears_zero(moving, 0.002f));
  moving[3] = -0.05f;   // through and out the other side
  EXPECT_TRUE(command_clears_zero(moving, 0.002f));
}

TEST(CommandClearsZero, TheThresholdIsOnMagnitudeNotSign) {
  // A negative joint is just as much "away from zero" as a positive one.
  EXPECT_TRUE(command_clears_zero({-0.5f, -0.5f}, 0.002f));
  EXPECT_FALSE(command_clears_zero({-0.001f, -0.5f}, 0.002f));
}

TEST(CommandClearsZero, TheBoundaryIsExclusive) {
  // Exactly at the threshold counts as still at zero, so a threshold of 0 does
  // not open the gate on a genuine 0.0.
  EXPECT_FALSE(command_clears_zero({0.002f, 0.5f}, 0.002f));
  EXPECT_TRUE(command_clears_zero({0.0021f, 0.5f}, 0.002f));
  EXPECT_FALSE(command_clears_zero({0.0f, 0.5f}, 0.0f));
}

TEST(CommandClearsZero, ANonFiniteElementGatesRatherThanOpens) {
  // A garbage read must close the gate, not open it: comparisons against NaN are
  // false, so a naive test would have let it through.
  EXPECT_FALSE(command_clears_zero({std::nanf(""), 0.5f}, 0.002f));
  EXPECT_FALSE(
    command_clears_zero({std::numeric_limits<float>::infinity(), 0.5f}, 0.002f));
}

TEST(HapticBaseline, ARepeatedOrBackwardTimestampDoesNotCorruptTheLevel) {
  // dt <= 0 must be skipped rather than trusted: a negative alpha would push the
  // level the wrong way.
  double t = 0.0;
  HapticBaseline b = settled(kRest, t);
  const float before = b.level();

  b.deviation_for(kRest + 3.0f, t);        // same timestamp
  b.deviation_for(kRest + 3.0f, t - 5.0);  // earlier timestamp
  EXPECT_NEAR(b.level(), before, 1e-3);
}

}  // namespace
