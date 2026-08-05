/**
 * @file test_video_encoder.cpp
 * @brief Unit tests for the per-frame Annex B video encoder and depth quantization.
 *
 * These pin down the properties that recorded video must have for the rest of the
 * pipeline to be correct, all of which fail silently rather than loudly if broken:
 *
 *   - One frame in yields exactly one packet out. The converter pairs camera
 *     frames to joint samples by index, so a dropped or doubled packet shifts
 *     every subsequent frame's alignment without any error being raised.
 *   - Packets are Annex B and repeat parameter sets on keyframes, as Foxglove's
 *     CompressedVideo requires and as per-episode remux depends on.
 *   - Depth quantization round-trips within one quantum, and matches lerobot's
 *     constants. A drift here decodes to plausible but wrong distances.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <opencv2/opencv.hpp>

#include "gtest/gtest.h"

#include "trossen_sdk/utils/depth_quantization.hpp"

#ifdef TROSSEN_ENABLE_VIDEO_ENCODE
#include "trossen_sdk/utils/video_encoder.hpp"
#endif

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 48;

/// @brief A frame with content that varies per index, so encoded sizes differ
///        and a stuck/duplicated frame would be visible.
cv::Mat make_color_frame(int index) {
  cv::Mat frame(kHeight, kWidth, CV_8UC3, cv::Scalar(0, 0, 0));
  cv::rectangle(frame, cv::Rect((index * 3) % (kWidth - 8), 4, 8, 8),
                cv::Scalar(20 + index, 200 - index, 128), cv::FILLED);
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

}  // namespace

// ============================================================================
// Depth quantization (always compiled; no ffmpeg needed)
// ============================================================================

TEST(DepthQuantization, MatchesLeRobotConstants) {
  EXPECT_EQ(trossen::utils::DEPTH_QUANT_BITS, 12);
  EXPECT_EQ(trossen::utils::DEPTH_QMAX, 4095);
  EXPECT_DOUBLE_EQ(trossen::utils::DEPTH_MIN_M, 0.01);
  EXPECT_DOUBLE_EQ(trossen::utils::DEPTH_MAX_M, 10.0);
  EXPECT_DOUBLE_EQ(trossen::utils::DEPTH_SHIFT_M, 3.5);
}

TEST(DepthQuantization, CodesStayInRange) {
  for (int mm : {0, 1, 10, 500, 1000, 4095, 10000, 20000, 65535}) {
    const uint16_t code = trossen::utils::quantize_depth_mm(static_cast<uint16_t>(mm));
    EXPECT_LE(code, trossen::utils::DEPTH_QMAX) << "mm=" << mm;
  }
}

TEST(DepthQuantization, IsMonotonic) {
  // Log quantization must never invert order, or nearer things read as farther.
  uint16_t previous = 0;
  for (int mm = 0; mm <= 12000; mm += 7) {
    const uint16_t code = trossen::utils::quantize_depth_mm(static_cast<uint16_t>(mm));
    EXPECT_GE(code, previous) << "mm=" << mm;
    previous = code;
  }
}

TEST(DepthQuantization, RoundTripsWithinOneQuantum) {
  // Within the representable band, dequantize(quantize(d)) must land close to d.
  // Tolerance is one quantum's worth of depth at that distance, which grows with
  // range because the mapping is logarithmic by design.
  for (int mm = 100; mm <= 9000; mm += 100) {
    const double metres = mm / 1000.0;
    const uint16_t code = trossen::utils::quantize_depth_mm(static_cast<uint16_t>(mm));
    const double back = trossen::utils::dequantize_depth_m(code);
    const double quantum =
      trossen::utils::dequantize_depth_m(static_cast<uint16_t>(code + 1)) - back;
    EXPECT_NEAR(back, metres, std::max(quantum, 1e-3)) << "mm=" << mm;
  }
}

TEST(DepthQuantization, LutAgreesWithScalarFunction) {
  // The recorder uses the LUT and the converter could use either; they must agree
  // for every possible input, not just sampled ones.
  const std::vector<uint16_t> lut = trossen::utils::build_depth_quantization_lut();
  ASSERT_EQ(lut.size(), 65536u);
  for (int d = 0; d < 65536; ++d) {
    ASSERT_EQ(lut[d], trossen::utils::quantize_depth_mm(static_cast<uint16_t>(d))) << "d=" << d;
  }
}

TEST(DepthQuantization, ClampsBeyondRange) {
  // Past depth_max everything saturates: a real limitation of the 12-bit range
  // that callers should know about rather than discover in training.
  const uint16_t at_max = trossen::utils::quantize_depth_mm(10000);
  const uint16_t beyond = trossen::utils::quantize_depth_mm(60000);
  EXPECT_EQ(beyond, trossen::utils::DEPTH_QMAX);
  EXPECT_LE(at_max, trossen::utils::DEPTH_QMAX);
}

// ============================================================================
// Video encoder (requires the ffmpeg-backed build)
// ============================================================================

#ifdef TROSSEN_ENABLE_VIDEO_ENCODE

using trossen::utils::VideoCodec;
using trossen::utils::VideoEncoder;

namespace {

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

TEST(VideoEncoderTest, FormatStringsMatchFoxgloveVocabulary) {
  EXPECT_STREQ(trossen::utils::video_codec_format(VideoCodec::H264), "h264");
  EXPECT_STREQ(trossen::utils::video_codec_format(VideoCodec::H265), "h265");
}

TEST(VideoEncoderTest, RejectsInvalidGeometry) {
  VideoEncoder::Params p = color_params();
  p.width = 0;
  EXPECT_EQ(VideoEncoder::create(p), nullptr);

  // H.264 chroma is subsampled 2x2, so odd dimensions cannot be represented.
  p = color_params();
  p.width = 65;
  EXPECT_EQ(VideoEncoder::create(p), nullptr);
}

TEST(VideoEncoderTest, RejectsLosslessOnH264) {
  VideoEncoder::Params p = color_params();
  p.lossless = true;  // lossless is a depth/H265 mode only
  EXPECT_EQ(VideoEncoder::create(p), nullptr);
}

TEST(VideoEncoderTest, EmitsExactlyOnePacketPerFrame) {
  // The invariant the converter's index-based alignment rests on.
  auto encoder = VideoEncoder::create(color_params());
  ASSERT_NE(encoder, nullptr);

  constexpr int kFrames = 40;  // several GOPs at gop_size=5
  int packets = 0;
  for (int i = 0; i < kFrames; ++i) {
    bool keyframe = false;
    const auto packet = encoder->encode(make_color_frame(i), keyframe);
    ASSERT_TRUE(packet.has_value()) << "no packet for frame " << i;
    EXPECT_GT(packet->size(), 0u) << "empty packet for frame " << i;
    ++packets;
  }
  EXPECT_EQ(packets, kFrames);
}

TEST(VideoEncoderTest, FirstFrameIsAKeyframe) {
  // An episode whose first packet is a delta frame is undecodable from its start.
  auto encoder = VideoEncoder::create(color_params());
  ASSERT_NE(encoder, nullptr);
  bool keyframe = false;
  ASSERT_TRUE(encoder->encode(make_color_frame(0), keyframe).has_value());
  EXPECT_TRUE(keyframe);
}

TEST(VideoEncoderTest, PacketsAreAnnexB) {
  auto encoder = VideoEncoder::create(color_params());
  ASSERT_NE(encoder, nullptr);
  for (int i = 0; i < 10; ++i) {
    bool keyframe = false;
    const auto packet = encoder->encode(make_color_frame(i), keyframe);
    ASSERT_TRUE(packet.has_value());
    EXPECT_EQ(find_start_code(*packet), 0u)
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
    bool keyframe = false;
    const auto packet = encoder->encode(make_color_frame(i), keyframe);
    ASSERT_TRUE(packet.has_value());
    if (!keyframe) continue;
    const std::vector<int> nals = h264_nal_types(*packet);
    // 7 = SPS, 8 = PPS, 5 = IDR slice.
    EXPECT_NE(std::find(nals.begin(), nals.end(), 7), nals.end())
      << "keyframe at " << i << " has no SPS";
    EXPECT_NE(std::find(nals.begin(), nals.end(), 8), nals.end())
      << "keyframe at " << i << " has no PPS";
    ++keyframes_checked;
  }
  EXPECT_GE(keyframes_checked, 2) << "expected repeating keyframes at gop_size=5";
}

TEST(VideoEncoderTest, HonorsGopSize) {
  auto params = color_params();
  params.gop_size = 5;
  auto encoder = VideoEncoder::create(params);
  ASSERT_NE(encoder, nullptr);

  std::vector<int> keyframe_indices;
  for (int i = 0; i < 20; ++i) {
    bool keyframe = false;
    ASSERT_TRUE(encoder->encode(make_color_frame(i), keyframe).has_value());
    if (keyframe) keyframe_indices.push_back(i);
  }
  // Exact placement is the encoder's business; what matters is that keyframes
  // recur on roughly the requested interval rather than only once at the start.
  ASSERT_GE(keyframe_indices.size(), 3u);
  EXPECT_EQ(keyframe_indices.front(), 0);
  EXPECT_LE(keyframe_indices[1], params.gop_size + 1);
}

TEST(VideoEncoderTest, RejectsWrongFrameSizeAndType) {
  auto encoder = VideoEncoder::create(color_params());
  ASSERT_NE(encoder, nullptr);

  bool keyframe = false;
  cv::Mat wrong_size(kHeight + 2, kWidth, CV_8UC3, cv::Scalar(0, 0, 0));
  EXPECT_FALSE(encoder->encode(wrong_size, keyframe).has_value());

  cv::Mat wrong_type(kHeight, kWidth, CV_16UC1, cv::Scalar(0));
  EXPECT_FALSE(encoder->encode(wrong_type, keyframe).has_value());
}

TEST(VideoEncoderTest, ReportsResolvedEncoderName) {
  auto encoder = VideoEncoder::create(color_params());
  ASSERT_NE(encoder, nullptr);
  EXPECT_EQ(encoder->encoder_name(), "libx264");
  EXPECT_EQ(encoder->codec(), VideoCodec::H264);
}

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
    cv::Mat depth(kHeight, kWidth, CV_16UC1);
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        const auto mm = static_cast<uint16_t>(500 + (x + y + i) * 10);
        depth.at<uint16_t>(y, x) = trossen::utils::quantize_depth_mm(mm);
      }
    }
    bool keyframe = false;
    const auto packet = encoder->encode(depth, keyframe);
    ASSERT_TRUE(packet.has_value()) << "no depth packet for frame " << i;
    EXPECT_GT(packet->size(), 0u);
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
    cv::Mat frame(480, 640, CV_8UC3, cv::Scalar(30, 60, 90));
    cv::circle(frame, cv::Point(100 + i * 5, 240), 40, cv::Scalar(200, 30, 30), cv::FILLED);
    bool keyframe = false;
    const auto packet = encoder->encode(frame, keyframe);
    ASSERT_TRUE(packet.has_value());
    encoded_total += packet->size();
  }
  const size_t raw_total = kRawBytesPerFrame * kFrames;
  EXPECT_LT(encoded_total, raw_total / 10)
    << "encoded " << encoded_total << " vs raw " << raw_total;
}

#endif  // TROSSEN_ENABLE_VIDEO_ENCODE
