/**
 * @file test_glide_haptics.cpp
 * @brief Unit tests for the Glide contact-force to vibration-intensity curve.
 */

#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

#include <gtest/gtest.h>

#include "trossen_sdk/hw/glide/glide_haptics.hpp"

namespace {

using trossen::hw::glide::GlideHapticCurve;

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

}  // namespace
