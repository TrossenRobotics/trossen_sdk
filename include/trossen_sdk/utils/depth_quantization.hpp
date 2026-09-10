/**
 * @file depth_quantization.hpp
 * @brief 16-bit depth -> 12-bit log-quantized codes, matching LeRobot's depth codec.
 *
 * LeRobot stores depth maps as lossless HEVC Main 12 `gray12le` video. Raw depth
 * does not fit in 12 bits, so it is first log-quantized over a configurable
 * metric range: fine resolution up close, coarse far away. Any producer of
 * LeRobot depth video must use exactly the mapping the consumer expects, or
 * dequantization returns wrong distances -- silently, with plausible-looking
 * output.
 *
 * This header is the single definition of that mapping, shared by the two
 * places that need it: the TrossenMCAP recorder (encoding depth at capture
 * time) and the offline LeRobot converter. Two independent copies would be
 * free to drift apart, and the failure mode is corrupt training data that
 * looks fine until a policy misjudges distance.
 *
 * depth_min/depth_max/depth_shift/use_log are parameters, defaulted to
 * lerobot 0.6.0's own `DepthEncoderConfig` values, so an unconfigured call
 * behaves identically to a dataset lerobot itself would produce.
 */

#ifndef TROSSEN_SDK__UTILS__DEPTH_QUANTIZATION_HPP_
#define TROSSEN_SDK__UTILS__DEPTH_QUANTIZATION_HPP_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace trossen::utils {

/// @brief Quantization bit depth. Fixed by the gray12le pixel format's
///   definition, not a tuning knob.
inline constexpr int DEPTH_QUANT_BITS = 12;

/// @brief Highest representable code, (1 << DEPTH_QUANT_BITS) - 1.
// 1 << 12 = 4096 (binary 1000000000000, i.e. 2^12); 4096 - 1 = 4095.
inline constexpr int DEPTH_QMAX = (1 << DEPTH_QUANT_BITS) - 1;

/// @brief Default depth (meters) mapped to code 0. Mirrors lerobot's
///   `DepthEncoderConfig.depth_min`; overridable via TrossenMCAPBackendConfig.
inline constexpr double DEFAULT_DEPTH_MIN_M = 0.01;

/// @brief Default depth (meters) mapped to DEPTH_QMAX. Mirrors lerobot's
///   `DepthEncoderConfig.depth_max`; overridable via TrossenMCAPBackendConfig.
inline constexpr double DEFAULT_DEPTH_MAX_M = 10.0;

/// @brief Default pre-log offset (meters), keeping ln() away from zero.
///   Mirrors lerobot's `DepthEncoderConfig.shift`; overridable via
///   TrossenMCAPBackendConfig.
inline constexpr double DEFAULT_DEPTH_SHIFT_M = 3.5;

/// @brief Default quantization mode. true selects log-space (finer
///   resolution close up); false selects plain linear. Mirrors lerobot's
///   `DepthEncoderConfig.use_log`.
inline constexpr bool DEFAULT_DEPTH_USE_LOG = true;

/// @brief Millimeters per meter. Raw depth_mm samples arrive in millimeters
///   (the sensor's native mono16/Z16 unit), while depth_min_m/depth_max_m/
///   depth_shift_m are in meters, matching lerobot's units and reading
///   naturally in config.json.
// Converting once here, per call, keeps that one unit boundary in a single
// place instead of hand-converting every constant/parameter to millimeters
// at its definition site -- and the conversion itself is negligible next to
// the log() calls below it.
inline constexpr double MM_PER_METER = 1000.0;

/**
 * @brief Log- or linear-quantize one raw depth sample in millimeters to a
 *   12-bit code.
 *
 * norm = (ln(d+shift) - ln(min+shift)) / (ln(max+shift) - ln(min+shift))   [use_log=true]
 * norm = (d - min) / (max - min)                                          [use_log=false]
 * code = clamp(round(norm * DEPTH_QMAX), 0, DEPTH_QMAX)
 *
 * @param depth_mm Raw depth in millimeters (mono16/Z16 sensor units). 0 is
 *   the sensor's standard "invalid pixel" value and is handled safely
 *   (clamps to code 0).
 * @param depth_min_m Depth in meters mapped to code 0.
 * @param depth_max_m Depth in meters mapped to code DEPTH_QMAX.
 * @param depth_shift_m Pre-log offset in meters, keeping ln() away from zero.
 * @param use_log If true (default), quantize in log space (finer resolution
 *   close up, matching real sensor accuracy); if false, quantize linearly.
 * @return Code in [0, DEPTH_QMAX].
 */
inline uint16_t quantize_depth_mm(uint16_t depth_mm,
                                  double depth_min_m = DEFAULT_DEPTH_MIN_M,
                                  double depth_max_m = DEFAULT_DEPTH_MAX_M,
                                  double depth_shift_m = DEFAULT_DEPTH_SHIFT_M,
                                  bool use_log = DEFAULT_DEPTH_USE_LOG) {
  // Sensor input (depth_mm) and shift/min/max are now in matching units
  // (millimeters); everything below operates in mm, so no further unit
  // conversion is needed past this point.
  const double min_mm = depth_min_m * MM_PER_METER;
  const double max_mm = depth_max_m * MM_PER_METER;
  const double shift_mm = depth_shift_m * MM_PER_METER;

  // norm is "where does this depth sample sit, as a fraction 0.0-1.0,
  // between min and max".  Everything after this just scales that fraction
  // into an integer code.
  double norm;
  if (use_log) {
    // Precompute once rather than inside the ratio below: log_min/log_max
    // don't depend on depth_mm, so this avoids two redundant log() calls
    // per sample.
    const double log_min = std::log(min_mm + shift_mm);
    const double log_max = std::log(max_mm + shift_mm);
    // shift guarantees this argument stays > 0 even when the sensor reports
    // exactly 0 (its standard "invalid pixel" convention). ln(0) is undefined,
    // ln(shift) is not. Taking the log also compresses far distances
    // and preserves resolution close up, matching where a real
    // depth sensor's own accuracy degrades.
    norm = (std::log(static_cast<double>(depth_mm) + shift_mm) - log_min) /
           (log_max - log_min);
  } else {
    // Plain proportion: equal meter-widths map to equal code-widths, no
    // compression. Only used when use_log is explicitly disabled.
    norm = (static_cast<double>(depth_mm) - min_mm) / (max_mm - min_mm);
  }
  // Scale the unitless [0,1] fraction up to an actual 12-bit integer.
  // llround (not a truncation cast) rounds to the nearest code instead of
  // always rounding down, which would bias every single value low.
  const int64_t code = std::llround(norm * DEPTH_QMAX);
  // Clamp before narrowing to uint16_t: sensor noise or a reading outside
  // [depth_min, depth_max] could push norm slightly below 0 or above 1.
  // Without clamping, that would silently wrap on the cast (e.g. 4096 -> 0),
  // corrupting the depth value with no error raised anywhere.
  return static_cast<uint16_t>(std::clamp<int64_t>(code, 0, DEPTH_QMAX));
}

/**
 * @brief Recover an approximate depth in meters from a 12-bit code.
 *
 * Inverse of quantize_depth_mm(): undoes the scale-to-code step, then the
 * normalize step, then (in log mode) the log itself via exp().
 * @param code 12-bit code in [0, DEPTH_QMAX], as produced by
 * quantize_depth_mm().
 * @param depth_min_m Depth in meters mapped to code 0. Must match the value
 * used to encode this code, or the recovered depth will be silently wrong.
 * @param depth_max_m Depth in meters mapped to DEPTH_QMAX. Must match the
 * encoder.
 * @param depth_shift_m Pre-log offset in meters used during encoding. Must
 * match.
 * @param use_log Must match the mode (log vs. linear) used to encode this code.
 * @return Approximate depth in meters.
 */
inline double dequantize_depth_m(uint16_t code,
                                 double depth_min_m = DEFAULT_DEPTH_MIN_M,
                                 double depth_max_m = DEFAULT_DEPTH_MAX_M,
                                 double depth_shift_m = DEFAULT_DEPTH_SHIFT_M,
                                 bool use_log = DEFAULT_DEPTH_USE_LOG) {
  // code / DEPTH_QMAX recovers norm, the [0,1] fraction quantize_depth_mm
  // produced. Cast to double first -- code and DEPTH_QMAX are both integer
  // types, so plain integer division would truncate to 0 for any code below
  // DEPTH_QMAX.
  const double norm = static_cast<double>(code) / DEPTH_QMAX;

  double depth_m;

  if (use_log) {
    // Inverse of the log-mode forward math, run backwards: undo the
    // normalization (scale by the log range, add back log_min), then undo
    // the log with exp(), then undo the shift.
    const double log_min = std::log(depth_min_m + depth_shift_m);
    const double log_max = std::log(depth_max_m + depth_shift_m);
    depth_m = std::exp(log_min + norm * (log_max - log_min)) - depth_shift_m;
  } else {
    // Inverse of the plain linear proportion.
    depth_m = depth_min_m + norm * (depth_max_m - depth_min_m);
  }
  return depth_m;
}

/**
 * @brief Precompute quantize_depth_mm() for every possible raw depth value.
 *
 * depth_mm is a uint16_t, so its entire domain is only 65536 values. Rather
 * than paying for std::log() on every pixel of every frame (~9.2M calls/sec
 * for a VGA depth stream at 30 Hz), compute the answer for every possible
 * input once and store it -- turning each later lookup into a single array
 * read instead of a log()-based computation.
 *
 * The table is only valid for the exact (depth_min_m, depth_max_m,
 * depth_shift_m, use_log) it was built with; a different configuration
 * needs its own table.
 *
 * @param depth_min_m Depth in meters mapped to code 0.
 * @param depth_max_m Depth in meters mapped to code DEPTH_QMAX.
 * @param depth_shift_m Pre-log offset in meters, keeping ln() away from zero.
 * @param use_log If true (default), quantize in log space; if false, linearly.
 * @return Table indexed by raw millimeter value (0-65535), yielding the
 *   corresponding 12-bit code.
 */
inline std::vector<uint16_t> build_depth_quantization_lut(double depth_min_m = DEFAULT_DEPTH_MIN_M,
                             double depth_max_m = DEFAULT_DEPTH_MAX_M,
                             double depth_shift_m = DEFAULT_DEPTH_SHIFT_M,
                             bool use_log = DEFAULT_DEPTH_USE_LOG) {
  // depth_mm's entire domain is 0-65535 (uint16_t), so a 65536-entry table
  // covers every possible input exactly once. Heap-allocated via std::vector
  // (128 KiB) rather than a stack array, and value-initialized to 0.
  std::vector<uint16_t> lut(65536);

  // Loop counter must be wider than uint16_t. If d were uint16_t, then at
  // d == 65535 the "++d" wraps back to 0 (unsigned overflow is well-defined
  // wraparound, not UB) -- "d < 65536" would then be true forever, since a
  // uint16_t can never actually hold 65536. int easily covers 0..65536, so
  // the loop terminates normally.
  for (int d = 0; d < 65536; ++d) {
    // Only cast down to uint16_t here, at the boundary where
    // quantize_depth_mm() actually needs that exact type -- this is the one
    // expensive call (the log()s) that this whole table exists to avoid
    // paying for again at runtime.
    lut[d] = quantize_depth_mm(static_cast<uint16_t>(d), depth_min_m, depth_max_m,
                                depth_shift_m, use_log);
  }

  // Moved out, not copied -- returning a local std::vector by value doesn't
  // duplicate the 65536 entries.
  return lut;
}

}  // namespace trossen::utils

#endif  // TROSSEN_SDK__UTILS__DEPTH_QUANTIZATION_HPP_
