/**
 * @file test_video_encoder.cpp
 * @brief Unit tests for the per-frame Annex B video encoder.
 *
 * These pin down the properties that recorded video must have for the rest of the
 * pipeline to be correct, all of which fail silently rather than loudly if broken:
 *
 *   - One frame in yields exactly one packet out. The converter pairs camera
 *     frames to joint samples by index, so a dropped or doubled packet shifts
 *     every subsequent frame's alignment without any error being raised.
 *   - Packets are Annex B and repeat parameter sets on keyframes, as Foxglove's
 *     CompressedVideo requires and as per-episode remux depends on.
 *   - Lossless depth encoding preserves the exact 12-bit quantized codes. Any
 *     loss there decodes to plausible but wrong distances.
 *
 * The quantization mapping itself is covered by test_depth_quantization.cpp;
 * this file only checks that the encoder carries the codes through intact.
 *
 * Split in two groups, matching the create()/encode() PR split: CREATE-ONLY TESTS
 * exercise VideoEncoder::create() alone, ENCODE TESTS exercise encode() as well.
 */

#include "gtest/gtest.h"

#include "trossen_sdk/utils/video_encoder.hpp"

namespace {

using trossen::utils::VideoCodec;
using trossen::utils::VideoEncoder;

}  // namespace

// ============================================================================
// CREATE-ONLY TESTS: exercise VideoEncoder::create() and video_codec_format().
// No test below this point calls encode().
// ============================================================================

// The exact strings CompressedVideo.format expects; a typo here would break
// Foxglove playback silently instead of failing a build.
TEST(VideoEncoderTest, FormatStringsMatchFoxgloveVocabulary) {
  EXPECT_STREQ(trossen::utils::video_codec_format(VideoCodec::H264), "h264");
  EXPECT_STREQ(trossen::utils::video_codec_format(VideoCodec::H265), "h265");
}
