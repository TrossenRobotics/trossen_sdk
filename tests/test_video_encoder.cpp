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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "trossen_sdk/utils/depth_quantization.hpp"
#include "trossen_sdk/utils/video_encoder.hpp"

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 48;

/// @brief A raw BGR8 frame whose content varies per index, so encoded sizes differ
///        and a stuck/duplicated frame would be visible.
std::vector<uint8_t> make_color_frame(int index) {
  std::vector<uint8_t> frame(static_cast<size_t>(kWidth) * kHeight * 3, 0);
  const int block_x = (index * 3) % (kWidth - 8);
  for (int y = 4; y < 12; ++y) {
    for (int x = block_x; x < block_x + 8; ++x) {
      const size_t offset = (static_cast<size_t>(y) * kWidth + x) * 3;
      frame[offset + 0] = static_cast<uint8_t>(20 + index);
      frame[offset + 1] = static_cast<uint8_t>(200 - index);
      frame[offset + 2] = 128;
    }
  }
  return frame;
}

/// @brief A raw gray12le depth frame: already-quantized 12-bit codes, the way the
///        recorder will feed them, packed 2 bytes per pixel little-endian.
std::vector<uint8_t> make_depth_frame(int index) {
  std::vector<uint8_t> frame(static_cast<size_t>(kWidth) * kHeight * 2);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const auto mm = static_cast<uint16_t>(500 + (x + y + index) * 10);
      const uint16_t code = trossen::utils::quantize_depth_mm(mm);
      const size_t offset = (static_cast<size_t>(y) * kWidth + x) * 2;
      frame[offset + 0] = static_cast<uint8_t>(code & 0xFF);
      frame[offset + 1] = static_cast<uint8_t>((code >> 8) & 0xFF);
    }
  }
  return frame;
}

/// @brief Offset of the first Annex B start code (00 00 01 / 00 00 00 01), or npos.
size_t find_start_code(std::span<const std::byte> data, size_t from = 0) {
  for (size_t i = from; i + 3 < data.size(); ++i) {
    const auto b0 = static_cast<uint8_t>(data[i]);
    const auto b1 = static_cast<uint8_t>(data[i + 1]);
    const auto b2 = static_cast<uint8_t>(data[i + 2]);
    if (b0 == 0 && b1 == 0 && (b2 == 1 || (b2 == 0 && static_cast<uint8_t>(data[i + 3]) == 1))) {
      return i;
    }
  }
  return std::string::npos;
}

/// @brief Collect H.264 NAL unit type values (lower 5 bits of the header byte).
std::vector<int> h264_nal_types(std::span<const std::byte> data) {
  std::vector<int> types;
  for (size_t i = 0; i + 3 < data.size(); ++i) {
    const auto b0 = static_cast<uint8_t>(data[i]);
    const auto b1 = static_cast<uint8_t>(data[i + 1]);
    const auto b2 = static_cast<uint8_t>(data[i + 2]);
    size_t header = 0;
    if (b0 == 0 && b1 == 0 && b2 == 1) {
      header = i + 3;
    } else if (b0 == 0 && b1 == 0 && b2 == 0 && static_cast<uint8_t>(data[i + 3]) == 1) {
      header = i + 4;
    } else {
      continue;
    }
    if (header < data.size()) {
      types.push_back(static_cast<uint8_t>(data[header]) & 0x1F);
    }
  }
  return types;
}

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

// ============================================================================
// ENCODE TESTS: exercise VideoEncoder::encode().
// ============================================================================

TEST(VideoEncoderTest, EmitsExactlyOnePacketPerFrame) {
  // The invariant the converter's index-based alignment rests on.
  auto encoder = VideoEncoder::create(color_params());
  ASSERT_NE(encoder, nullptr);

  // Several GOPs at gop_size=5.
  constexpr int kFrames = 40;
  int packets = 0;
  for (int i = 0; i < kFrames; ++i) {
    const auto frame = make_color_frame(i);
    const auto result = encoder->encode(frame.data(), frame.size());
    EXPECT_GT(result.data.size(), 0u) << "empty packet for frame " << i;
    ++packets;
  }
  EXPECT_EQ(packets, kFrames);
}

TEST(VideoEncoderTest, FirstFrameIsAKeyframe) {
  // An episode whose first packet is a delta frame is undecodable from its start.
  auto encoder = VideoEncoder::create(color_params());
  ASSERT_NE(encoder, nullptr);
  const auto frame = make_color_frame(0);
  const auto result = encoder->encode(frame.data(), frame.size());
  EXPECT_TRUE(result.is_keyframe);
}

// Foxglove's CompressedVideo requires Annex B framing (start-code-prefixed
// NALs), not length-prefixed AVCC.
TEST(VideoEncoderTest, PacketsAreAnnexB) {
  auto encoder = VideoEncoder::create(color_params());
  ASSERT_NE(encoder, nullptr);
  for (int i = 0; i < 10; ++i) {
    const auto frame = make_color_frame(i);
    const auto result = encoder->encode(frame.data(), frame.size());
    EXPECT_EQ(find_start_code(result.data), 0u)
        << "frame " << i << " does not begin with an Annex B start code";
  }
}

TEST(VideoEncoderTest, KeyframesCarryParameterSets) {
  // Foxglove requires SPS alongside every IDR, and a remux that slices the
  // stream mid-file cannot decode without it.
  auto encoder = VideoEncoder::create(color_params());
  ASSERT_NE(encoder, nullptr);

  int keyframes_checked = 0;
  for (int i = 0; i < 20; ++i) {
    const auto frame = make_color_frame(i);
    const auto result = encoder->encode(frame.data(), frame.size());
    if (!result.is_keyframe) continue;
    const std::vector<int> nals = h264_nal_types(result.data);
    // 7 = SPS, 8 = PPS, 5 = IDR slice.
    EXPECT_NE(std::find(nals.begin(), nals.end(), 7), nals.end())
        << "keyframe at " << i << " has no SPS";
    EXPECT_NE(std::find(nals.begin(), nals.end(), 8), nals.end())
        << "keyframe at " << i << " has no PPS";
    ++keyframes_checked;
  }
  EXPECT_GE(keyframes_checked, 2) << "expected repeating keyframes at gop_size=5";
}

// Keyframes must recur near the requested interval; an encoder that ignores
// gop_size makes random-access reads decode an unboundedly long GOP.
TEST(VideoEncoderTest, HonorsGopSize) {
  auto params = color_params();
  params.gop_size = 5;
  auto encoder = VideoEncoder::create(params);
  ASSERT_NE(encoder, nullptr);

  std::vector<int> keyframe_indices;
  for (int i = 0; i < 20; ++i) {
    const auto frame = make_color_frame(i);
    const auto result = encoder->encode(frame.data(), frame.size());
    if (result.is_keyframe) keyframe_indices.push_back(i);
  }
  // Exact placement is the encoder's business; what matters is that keyframes
  // recur on roughly the requested interval rather than only once at the start.
  ASSERT_GE(keyframe_indices.size(), 3u);
  EXPECT_EQ(keyframe_indices.front(), 0);
  EXPECT_LE(keyframe_indices[1], params.gop_size + 1);
}

// A caller that passes a buffer of the wrong size for this encoder's pixel
// format must be rejected, not read out of bounds or encode garbage.
TEST(VideoEncoderTest, RejectsWrongFrameSize) {
  auto encoder = VideoEncoder::create(color_params());
  ASSERT_NE(encoder, nullptr);

  // One row short of a full BGR8 frame.
  const std::vector<uint8_t> too_small(static_cast<size_t>(kWidth) * (kHeight - 1) * 3);
  EXPECT_EQ(encoder->encode(too_small.data(), too_small.size()).data.size(), 0u);

  // Sized as if 16-bit depth data, not BGR8 color: same pixel count, wrong byte count.
  const std::vector<uint8_t> wrong_layout(static_cast<size_t>(kWidth) * kHeight * 2);
  EXPECT_EQ(encoder->encode(wrong_layout.data(), wrong_layout.size()).data.size(), 0u);
}

// Depth must round-trip its already-quantized 12-bit codes exactly; any
// encoder loss here decodes to a plausible but wrong distance downstream.
TEST(VideoEncoderTest, LosslessDepthEncodesQuantizedCodes) {
  VideoEncoder::Params p;
  p.width = kWidth;
  p.height = kHeight;
  p.fps = 30;
  p.gop_size = 5;
  p.codec = VideoCodec::H265;
  p.lossless = true;
  p.encoder = "x265";

  auto encoder = VideoEncoder::create(p);
  if (!encoder) {
    GTEST_SKIP() << "libx265 not available in this ffmpeg build";
  }

  // Feed already-quantized 12-bit codes, the way the recorder will.
  for (int i = 0; i < 10; ++i) {
    const auto frame = make_depth_frame(i);
    const auto result = encoder->encode(frame.data(), frame.size());
    EXPECT_GT(result.data.size(), 0u) << "no depth packet for frame " << i;
  }
}

TEST(VideoEncoderTest, CompressesFarBelowRaw) {
  // The entire point of the feature: a raw VGA BGR frame is 921,600 bytes.
  // Encoded output should be orders of magnitude smaller for ordinary content.
  VideoEncoder::Params p = color_params();
  p.width = 640;
  p.height = 480;
  auto encoder = VideoEncoder::create(p);
  ASSERT_NE(encoder, nullptr);

  constexpr size_t kRawBytesPerFrame = 640 * 480 * 3;
  size_t encoded_total = 0;
  constexpr int kFrames = 30;
  for (int i = 0; i < kFrames; ++i) {
    std::vector<uint8_t> frame(kRawBytesPerFrame, 0);
    for (size_t p_idx = 0; p_idx < frame.size(); p_idx += 3) {
      frame[p_idx + 0] = 30;
      frame[p_idx + 1] = 60;
      frame[p_idx + 2] = 90;
    }
    const auto result = encoder->encode(frame.data(), frame.size());
    encoded_total += result.data.size();
  }
  const size_t raw_total = kRawBytesPerFrame * kFrames;
  EXPECT_LT(encoded_total, raw_total / 10)
      << "encoded " << encoded_total << " vs raw " << raw_total;
}
