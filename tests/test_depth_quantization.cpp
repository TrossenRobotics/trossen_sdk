/**
 * @file test_depth_quantization.cpp
 * @brief Unit tests for the shared LeRobot depth quantization mapping.
 *
 * The mapping in depth_quantization.hpp has no in-tree consumer yet: the
 * TrossenMCAP video recorder and the offline LeRobot v3 converter both arrive
 * later. It is pinned here regardless, because the failure mode it guards
 * against is silent. A producer that quantizes depth even slightly differently
 * from lerobot's `quantize_depth` still writes a well-formed gray12le video;
 * the error only surfaces as a policy misjudging distance after training. So
 * these tests assert the mapping's endpoints and invariants against the lerobot
 * 0.6.0 constants directly, rather than against whatever the code happens to do.
 */

#include <cmath>
#include <cstdint>
#include <vector>

#include "gtest/gtest.h"

#include "trossen_sdk/utils/depth_quantization.hpp"

using trossen::utils::build_depth_quantization_lut;
using trossen::utils::dequantize_depth_m;
using trossen::utils::DEFAULT_DEPTH_MAX_M;
using trossen::utils::DEFAULT_DEPTH_MIN_M;
using trossen::utils::DEPTH_QMAX;
using trossen::utils::DEPTH_QUANT_BITS;
using trossen::utils::DEFAULT_DEPTH_SHIFT_M;
using trossen::utils::MM_PER_METER;
using trossen::utils::quantize_depth_mm;

// ============================================================================
// Constants — these must match lerobot 0.6.0 or datasets decode incorrectly
// ============================================================================

TEST(DepthQuantizationTest, ConstantsMatchLerobotDefaults) {
  EXPECT_EQ(DEPTH_QUANT_BITS, 12);
  EXPECT_EQ(DEPTH_QMAX, 4095);
  EXPECT_DOUBLE_EQ(DEFAULT_DEPTH_MIN_M, 0.01);
  EXPECT_DOUBLE_EQ(DEFAULT_DEPTH_MAX_M, 10.0);
  EXPECT_DOUBLE_EQ(DEFAULT_DEPTH_SHIFT_M, 3.5);
}

TEST(DepthQuantizationTest, QmaxIsDerivedFromBitDepth) {
  EXPECT_EQ(DEPTH_QMAX, (1 << DEPTH_QUANT_BITS) - 1);
}

// ============================================================================
// quantize_depth_mm() endpoints
// ============================================================================

// depth_min maps to code 0 by construction: it is the numerator's zero point.
TEST(DepthQuantizationTest, DepthMinMapsToZero) {
  const uint16_t min_mm = static_cast<uint16_t>(DEFAULT_DEPTH_MIN_M * MM_PER_METER);
  EXPECT_EQ(quantize_depth_mm(min_mm), 0);
}

// depth_max maps to the top code, so the full 12-bit range is used.
TEST(DepthQuantizationTest, DepthMaxMapsToQmax) {
  const uint16_t max_mm = static_cast<uint16_t>(DEFAULT_DEPTH_MAX_M * MM_PER_METER);
  EXPECT_EQ(quantize_depth_mm(max_mm), DEPTH_QMAX);
}

// Zero is the RealSense "no return" value and sits below depth_min, so it must
// clamp rather than produce a negative code.
TEST(DepthQuantizationTest, ZeroDepthClampsToZero) {
  EXPECT_EQ(quantize_depth_mm(0), 0);
}

// Anything past depth_max clamps instead of wrapping. 65535 mm is the largest
// value a mono16 frame can carry, ~6.5x depth_max.
TEST(DepthQuantizationTest, BeyondDepthMaxClampsToQmax) {
  EXPECT_EQ(quantize_depth_mm(20000), DEPTH_QMAX);
  EXPECT_EQ(quantize_depth_mm(65535), DEPTH_QMAX);
}

// ============================================================================
// Invariants across the whole mono16 domain
// ============================================================================

// Log quantization is monotonic, so a nearer sample never gets a higher code.
// A break here means depth ordering is scrambled, which no consumer can detect.
TEST(DepthQuantizationTest, MappingIsMonotonicNonDecreasing) {
  uint16_t previous = quantize_depth_mm(0);
  for (int d = 1; d < 65536; ++d) {
    const uint16_t current = quantize_depth_mm(static_cast<uint16_t>(d));
    ASSERT_GE(current, previous) << "non-monotonic at " << d << " mm";
    previous = current;
  }
}

TEST(DepthQuantizationTest, EveryCodeIsInRange) {
  for (int d = 0; d < 65536; ++d) {
    const uint16_t code = quantize_depth_mm(static_cast<uint16_t>(d));
    ASSERT_LE(code, DEPTH_QMAX) << "code out of 12-bit range at " << d << " mm";
  }
}

// Resolution is finer up close than far away — the entire reason for using a
// log mapping rather than a linear one.
TEST(DepthQuantizationTest, ResolutionIsFinerNearThanFar) {
  const int codes_near = quantize_depth_mm(1100) - quantize_depth_mm(1000);
  const int codes_far = quantize_depth_mm(9100) - quantize_depth_mm(9000);
  EXPECT_GT(codes_near, codes_far);
}

// ============================================================================
// Lookup table
// ============================================================================

TEST(DepthQuantizationTest, LutCoversTheFullMono16Domain) {
  const std::vector<uint16_t> lut = build_depth_quantization_lut();
  ASSERT_EQ(lut.size(), 65536u);
}

// The LUT exists only as a speed optimization over the scalar function, so it
// must agree with it exactly — not approximately.
TEST(DepthQuantizationTest, LutMatchesScalarFunctionExactly) {
  const std::vector<uint16_t> lut = build_depth_quantization_lut();
  for (int d = 0; d < 65536; ++d) {
    ASSERT_EQ(lut[d], quantize_depth_mm(static_cast<uint16_t>(d)))
      << "LUT disagrees with quantize_depth_mm at " << d << " mm";
  }
}

// ============================================================================
// Round trip
// ============================================================================

TEST(DepthQuantizationTest, DequantizeInvertsTheEndpoints) {
  EXPECT_NEAR(dequantize_depth_m(0), DEFAULT_DEPTH_MIN_M, 1e-9);
  EXPECT_NEAR(dequantize_depth_m(DEPTH_QMAX), DEFAULT_DEPTH_MAX_M, 1e-9);
}

// Round-trip error must stay within one quantization step. The step is not
// constant: in log space it is uniform, so in metres it grows with distance as
// (depth + shift) * log(range) / QMAX. Asserting a single absolute tolerance
// would either be too loose up close or fail far away, so the bound is
// computed per sample.
TEST(DepthQuantizationTest, RoundTripStaysWithinOneQuantizationStep) {
  const double log_span =
    std::log(DEFAULT_DEPTH_MAX_M + DEFAULT_DEPTH_SHIFT_M) -
    std::log(DEFAULT_DEPTH_MIN_M + DEFAULT_DEPTH_SHIFT_M);
  const double log_step = log_span / static_cast<double>(DEPTH_QMAX);

  for (int mm = 10; mm <= 10000; mm += 10) {
    const double expected_m = static_cast<double>(mm) / MM_PER_METER;
    const double actual_m = dequantize_depth_m(quantize_depth_mm(static_cast<uint16_t>(mm)));
    // Half a step is the best a round-to-nearest quantizer can do; allow a
    // whole step for floating-point slack at the rounding boundaries.
    const double tolerance_m = (expected_m + DEFAULT_DEPTH_SHIFT_M) * log_step;
    ASSERT_NEAR(actual_m, expected_m, tolerance_m) << "at " << mm << " mm";
  }
}

// Depth beyond the encodable range saturates, so the round trip returns
// depth_max rather than the original distance. Callers must not read a
// saturated code as a real measurement.
TEST(DepthQuantizationTest, RoundTripSaturatesBeyondDepthMax) {
  EXPECT_NEAR(dequantize_depth_m(quantize_depth_mm(30000)), DEFAULT_DEPTH_MAX_M, 1e-9);
}

// ============================================================================
// Configurable parameters — depth_min_m/depth_max_m/depth_shift_m/use_log are
// runtime arguments now, not fixed constants. Every test above calls the
// functions with no arguments at all, so none of them would notice if a
// parameter were silently ignored in favor of its default. These do.
// ============================================================================

// use_log selects between two genuinely different formulas (log-space vs.
// linear). This proves the linear branch is actually reachable and produces
// a different answer, not dead code that happens to agree with the default.
// The expected codes are pinned to the exact values hand-verified earlier
// (1000 mm -> 755 log, 406 linear), not just "whatever the code computes."
TEST(DepthQuantizationTest, UseLogFalseDivergesFromDefaultLogMode) {
  const uint16_t log_code = quantize_depth_mm(1000);
  const uint16_t linear_code = quantize_depth_mm(
      1000, DEFAULT_DEPTH_MIN_M, DEFAULT_DEPTH_MAX_M, DEFAULT_DEPTH_SHIFT_M, false);
  EXPECT_NE(log_code, linear_code);
  EXPECT_EQ(log_code, 755);
  EXPECT_EQ(linear_code, 406);
}

// A custom depth_min_m/depth_max_m must actually move where the endpoints
// land -- the whole reason these became parameters instead of fixed
// constants is per-camera/per-deployment range configuration.
TEST(DepthQuantizationTest, CustomRangeMovesTheEndpoints) {
  constexpr double custom_min_m = 1.0;
  constexpr double custom_max_m = 5.0;
  const uint16_t min_mm = static_cast<uint16_t>(custom_min_m * MM_PER_METER);
  const uint16_t max_mm = static_cast<uint16_t>(custom_max_m * MM_PER_METER);

  EXPECT_EQ(quantize_depth_mm(min_mm, custom_min_m, custom_max_m), 0);
  EXPECT_EQ(quantize_depth_mm(max_mm, custom_min_m, custom_max_m), DEPTH_QMAX);

  // A midpoint value, unclamped under either range, must produce different
  // codes depending on which range it is measured against -- proves the
  // custom values are actually driving the computation, not just being
  // accepted and discarded in favor of the defaults. (A value near either
  // range's boundary would risk both sides clamping to the same code,
  // proving nothing -- that was the bug in this test's first draft.)
  // 3.0 m: inside both [1,5] and [0.01,10].
  constexpr uint16_t midpoint_mm = 3000;
  const uint16_t code_under_custom = quantize_depth_mm(midpoint_mm, custom_min_m, custom_max_m);
  const uint16_t code_under_default = quantize_depth_mm(midpoint_mm);
  EXPECT_NE(code_under_custom, code_under_default);
}

// The round-trip guarantee must hold for any valid configuration, not just
// the lerobot defaults. Mirrors RoundTripStaysWithinOneQuantizationStep's
// per-sample tolerance derivation, applied to a custom range/shift instead.
TEST(DepthQuantizationTest, RoundTripHoldsForCustomParameters) {
  constexpr double custom_min_m = 0.1;
  constexpr double custom_max_m = 3.0;
  constexpr double custom_shift_m = 1.0;

  const double log_span =
      std::log(custom_max_m + custom_shift_m) - std::log(custom_min_m + custom_shift_m);
  const double log_step = log_span / static_cast<double>(DEPTH_QMAX);

  for (int mm = 100; mm <= 3000; mm += 100) {
    const uint16_t code = quantize_depth_mm(
        static_cast<uint16_t>(mm), custom_min_m, custom_max_m, custom_shift_m);
    const double recovered_m =
        dequantize_depth_m(code, custom_min_m, custom_max_m, custom_shift_m);
    const double expected_m = static_cast<double>(mm) / MM_PER_METER;
    const double tolerance_m = (expected_m + custom_shift_m) * log_step;
    EXPECT_NEAR(recovered_m, expected_m, tolerance_m) << "at " << mm << " mm";
  }
}
