/**
 * @file depth_quantization.hpp
 * @brief 16-bit depth → 12-bit log-quantized codes, matching LeRobot's depth codec.
 *
 * LeRobot stores depth maps as lossless HEVC Main 12 `gray12le` video. Raw depth
 * does not fit in 12 bits, so it is first log-quantized over a fixed metric range:
 * fine resolution up close, coarse far away. Any producer of LeRobot depth video
 * must use exactly this mapping, or the consumer's dequantization returns wrong
 * distances — silently, with plausible-looking output.
 *
 * This header is the single definition of that mapping, shared by the two places
 * that need it: the TrossenMCAP recorder (encoding depth at capture time) and the
 * offline LeRobot converter. Two independent copies would be free to drift apart,
 * and the failure mode is corrupt training data that looks fine until a policy
 * misjudges distance.
 *
 * The constants mirror lerobot 0.6.0 `DepthEncoderConfig` / `quantize_depth`:
 * 12-bit codes (0..4095), depth_min 0.01 m, depth_max 10.0 m, shift 3.5 m,
 * logarithmic. They are deliberately not configurable — a dataset written with
 * different parameters is one LeRobot decodes incorrectly.
 */

#ifndef TROSSEN_SDK__UTILS__DEPTH_QUANTIZATION_HPP_
#define TROSSEN_SDK__UTILS__DEPTH_QUANTIZATION_HPP_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace trossen::utils {

/// @brief Quantization bit depth; matches lerobot DEPTH_QUANT_BITS.
inline constexpr int DEPTH_QUANT_BITS = 12;
/// @brief Highest code, (1 << DEPTH_QUANT_BITS) - 1; matches lerobot DEPTH_QMAX.
inline constexpr int DEPTH_QMAX = (1 << DEPTH_QUANT_BITS) - 1;

/// @brief Depth (metres) mapped to code 0; lerobot DEFAULT_DEPTH_MIN.
inline constexpr double DEPTH_MIN_M = 0.01;
/// @brief Depth (metres) mapped to DEPTH_QMAX; lerobot DEFAULT_DEPTH_MAX.
inline constexpr double DEPTH_MAX_M = 10.0;
/// @brief Pre-log offset (metres) keeping log() finite near zero; lerobot DEFAULT_DEPTH_SHIFT.
inline constexpr double DEPTH_SHIFT_M = 3.5;

/// @brief Millimetres per metre. Raw mono16 depth frames are in millimetres,
///        while the lerobot quantization parameters above are in metres.
inline constexpr double MM_PER_METRE = 1000.0;

/**
 * @brief Log-quantize one raw depth sample in millimetres to a 12-bit code.
 *
 * Mirrors lerobot's `quantize_depth(..., use_log=True)` with the parameters
 * converted to millimetres:
 *   norm = (log(d + shift) - log(min + shift)) / (log(max + shift) - log(min + shift))
 *   code = clamp(round(norm * DEPTH_QMAX), 0, DEPTH_QMAX)
 *
 * @param depth_mm Raw depth in millimetres (mono16 / Z16 units).
 * @return Code in [0, DEPTH_QMAX].
 */
inline uint16_t quantize_depth_mm(uint16_t depth_mm) {
  constexpr double shift_mm = DEPTH_SHIFT_M * MM_PER_METRE;
  constexpr double min_mm = DEPTH_MIN_M * MM_PER_METRE;
  constexpr double max_mm = DEPTH_MAX_M * MM_PER_METRE;
  const double log_min = std::log(min_mm + shift_mm);
  const double inv_range = 1.0 / (std::log(max_mm + shift_mm) - log_min);
  const double norm = (std::log(static_cast<double>(depth_mm) + shift_mm) - log_min) * inv_range;
  const int64_t code = std::llround(norm * DEPTH_QMAX);
  return static_cast<uint16_t>(std::clamp<int64_t>(code, 0, DEPTH_QMAX));
}

/**
 * @brief Build the full mono16 → 12-bit-code lookup table.
 *
 * Raw depth is uint16, so the domain is small enough to precompute exhaustively
 * (65536 entries, 128 KiB). Per-pixel `log()` on a VGA depth stream at 30 Hz is
 * ~9.2 M calls/second; the table turns that into a load.
 *
 * @return Table indexed by raw millimetre value, yielding the 12-bit code.
 */
inline std::vector<uint16_t> build_depth_quantization_lut() {
  std::vector<uint16_t> lut(65536);
  for (int d = 0; d < 65536; ++d) {
    lut[d] = quantize_depth_mm(static_cast<uint16_t>(d));
  }
  return lut;
}

/**
 * @brief Dequantize a 12-bit code back to depth in metres.
 *
 * Inverse of quantize_depth_mm(), matching lerobot's `dequantize_depth`. Provided
 * for round-trip verification rather than for the write path.
 *
 * @param code 12-bit code in [0, DEPTH_QMAX].
 * @return Depth in metres.
 */
inline double dequantize_depth_m(uint16_t code) {
  const double log_min = std::log(DEPTH_MIN_M + DEPTH_SHIFT_M);
  const double log_max = std::log(DEPTH_MAX_M + DEPTH_SHIFT_M);
  const double norm = static_cast<double>(code) / static_cast<double>(DEPTH_QMAX);
  return std::exp(log_min + norm * (log_max - log_min)) - DEPTH_SHIFT_M;
}

}  // namespace trossen::utils

#endif  // TROSSEN_SDK__UTILS__DEPTH_QUANTIZATION_HPP_
