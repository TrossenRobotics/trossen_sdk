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

#include "gtest/gtest.h"

#include "trossen_sdk/utils/depth_quantization.hpp"

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
