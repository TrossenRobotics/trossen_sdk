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

constexpr int kWidth = 64;
constexpr int kHeight = 48;

using trossen::utils::VideoCodec;
using trossen::utils::VideoEncoder;

VideoEncoder::Params color_params() {
  VideoEncoder::Params p;
  p.width = kWidth;
  p.height = kHeight;
  p.fps = 30;
  p.bitrate_kbps = 2000;
  p.gop_size = 5;
  p.codec = VideoCodec::H264;
  // Pin the software encoder: hardware availability varies per machine and this
  // test is about bitstream shape, not about which encoder a rig happens to have.
  p.encoder = "x264";
  return p;
}

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

// create() must fail fast on geometry it can never open an encoder for,
// rather than opening one that produces corrupt or undecodable output.
TEST(VideoEncoderTest, RejectsInvalidGeometry) {
  VideoEncoder::Params p = color_params();
  p.width = 0;
  EXPECT_EQ(VideoEncoder::create(p), nullptr);

  // H.264 chroma is subsampled 2x2, so odd dimensions cannot be represented.
  p = color_params();
  p.width = 65;
  EXPECT_EQ(VideoEncoder::create(p), nullptr);
}

// lossless is a depth/H265 mode only; silently accepting it on H264 would
// mean the flag gets ignored instead of the request being rejected.
TEST(VideoEncoderTest, RejectsLosslessOnH264) {
  VideoEncoder::Params p = color_params();
  p.lossless = true;
  EXPECT_EQ(VideoEncoder::create(p), nullptr);
}

// encoder_name() must report what was actually opened (e.g. "libx264"), not
// just the requested preference, since callers log/branch on the real name.
TEST(VideoEncoderTest, ReportsResolvedEncoderName) {
  auto encoder = VideoEncoder::create(color_params());
  ASSERT_NE(encoder, nullptr);
  EXPECT_EQ(encoder->encoder_name(), "libx264");
  EXPECT_EQ(encoder->codec(), VideoCodec::H264);
}
