/**
 * @file video_encoder.cpp
 * @brief libavcodec implementation of the per-frame Annex B video encoder.
 */

#include "trossen_sdk/utils/video_encoder.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace trossen::utils {

namespace {

/// @brief Hardware-first probe order for each codec, ending in the software encoder.
///
/// Software last so a machine without a usable GPU still records; the resolved
/// name is logged so it is obvious which one won on a given rig.
const std::vector<std::string>& probe_order(VideoCodec codec) {
  static const std::vector<std::string> kH264 = {"h264_nvenc", "h264_vaapi", "libx264"};
  // Depth is lossless Main 12. NVENC/VAAPI do not offer lossless 12-bit
  // monochrome, so there is nothing to probe: libx265 is the only option.
  static const std::vector<std::string> kH265 = {"libx265"};
  return codec == VideoCodec::H264 ? kH264 : kH265;
}

/// @brief Expand a friendly encoder alias to concrete libavcodec encoder names.
std::vector<std::string> resolve_candidates(const VideoEncoder::Params& params) {
  const std::string& want = params.encoder;
  if (want.empty() || want == "auto") {
    return probe_order(params.codec);
  }
  const bool h264 = params.codec == VideoCodec::H264;
  if (want == "nvenc") return {h264 ? "h264_nvenc" : "hevc_nvenc"};
  if (want == "vaapi") return {h264 ? "h264_vaapi" : "hevc_vaapi"};
  if (want == "x264") return {"libx264"};
  if (want == "x265") return {"libx265"};
  // Assume a literal libavcodec encoder name.
  return {want};
}

/// @brief Set a private encoder option, ignoring encoders that do not have it.
void try_set_opt(AVCodecContext* ctx, const char* key, const char* value) {
  // AV_OPT_SEARCH_CHILDREN reaches the encoder's private class (x264-params,
  // x265-params, nvenc presets). A missing key is expected across encoders and
  // is not an error worth surfacing.
  av_opt_set(ctx->priv_data, key, value, 0);
}

}  // namespace

const char* video_codec_format(VideoCodec codec) {
  return codec == VideoCodec::H264 ? "h264" : "h265";
}

/// @brief libavcodec state for one open encoder.
struct VideoEncoder::Impl {
  const AVCodec* codec{nullptr};
  AVCodecContext* ctx{nullptr};
  AVFrame* frame{nullptr};
  AVPacket* packet{nullptr};
  SwsContext* sws{nullptr};
  /// @brief Monotonic presentation timestamp, in stream time base units.
  int64_t pts{0};
  /// @brief Packet payload copied out of the AVPacket so the view stays valid
  ///        until the next encode() call, independent of libavcodec's refcounting.
  std::vector<std::byte> payload;

  ~Impl() {
    if (sws) sws_freeContext(sws);
    if (packet) av_packet_free(&packet);
    if (frame) av_frame_free(&frame);
    if (ctx) avcodec_free_context(&ctx);
  }
};

VideoEncoder::VideoEncoder() : impl_(std::make_unique<Impl>()) {}
VideoEncoder::~VideoEncoder() = default;

std::unique_ptr<VideoEncoder> VideoEncoder::create(const Params& params) {
  if (params.width <= 0 || params.height <= 0) {
    std::cerr << "VideoEncoder: invalid frame size " << params.width << "x" << params.height
              << "\n";
    return nullptr;
  }
  if (params.lossless && params.codec != VideoCodec::H265) {
    std::cerr << "VideoEncoder: lossless mode requires H265 (Main 12)\n";
    return nullptr;
  }
  // H.264 encoders reject odd dimensions in yuv420p (chroma is subsampled 2x2).
  if (params.codec == VideoCodec::H264 && (params.width % 2 || params.height % 2)) {
    std::cerr << "VideoEncoder: H264 needs even dimensions, got " << params.width << "x"
              << params.height << "\n";
    return nullptr;
  }

  const AVPixelFormat pix_fmt =
    params.codec == VideoCodec::H264 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_GRAY12LE;

  auto self = std::unique_ptr<VideoEncoder>(new VideoEncoder());
  self->codec_ = params.codec;
  Impl& impl = *self->impl_;

  for (const std::string& name : resolve_candidates(params)) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec) continue;

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) continue;

    ctx->width = params.width;
    ctx->height = params.height;
    ctx->pix_fmt = pix_fmt;
    ctx->time_base = AVRational{1, std::max(1, params.fps)};
    ctx->framerate = AVRational{std::max(1, params.fps), 1};
    ctx->gop_size = std::max(1, params.gop_size);
    // No B-frames: Foxglove's CompressedVideo explicitly does not support them,
    // and they would also reorder output relative to input, breaking the 1:1
    // frame-to-message pairing the converter's index alignment depends on.
    ctx->max_b_frames = 0;
    ctx->has_b_frames = 0;
    if (!params.lossless) {
      ctx->bit_rate = static_cast<int64_t>(params.bitrate_kbps) * 1000;
    }
    // Repeat SPS/PPS (and VPS) on every keyframe. Without this the parameter
    // sets appear only in extradata, so a consumer that starts reading mid-file
    // -- or a per-episode remux that slices the stream -- cannot decode.
    ctx->flags2 |= AV_CODEC_FLAG2_LOCAL_HEADER;

    // Every encoder below must be configured for *zero frame delay*: one
    // submitted frame has to produce its packet immediately. Frame-level
    // threading and lookahead both buffer frames internally, which would make
    // encode() return nothing for the first N frames and then run a frame
    // behind -- silently shifting the image-to-joint-state index pairing that
    // the converter relies on. Slice-level threading keeps the parallelism
    // without the delay, so prefer it over capping thread counts.
    if (name == "libx264") {
      try_set_opt(ctx, "preset", "veryfast");
      // zerolatency turns off lookahead and switches to sliced threads. Do not
      // override sliced-threads here: with frame threads, x264 withholds the
      // first packets until its pipeline fills.
      try_set_opt(ctx, "tune", "zerolatency");
      // repeat-headers mirrors flags2 LOCAL_HEADER for x264 specifically;
      // bframes=0 guards against preset defaults reintroducing them.
      try_set_opt(ctx, "x264-params", "repeat-headers=1:bframes=0");
    } else if (name == "libx265") {
      try_set_opt(ctx, "preset", "ultrafast");
      try_set_opt(ctx, "tune", "zerolatency");
      // x265 keeps a lookahead queue and frame threads even at ultrafast, so
      // both have to be disabled explicitly for one-in/one-out behaviour.
      std::string x265 =
        "repeat-headers=1:bframes=0:rc-lookahead=0:frame-threads=1:log-level=error";
      if (params.lossless) x265 += ":lossless=1";
      try_set_opt(ctx, "x265-params", x265.c_str());
    } else if (name.find("nvenc") != std::string::npos) {
      try_set_opt(ctx, "preset", "p1");
      try_set_opt(ctx, "tune", "ull");            // ultra-low latency: no lookahead
      try_set_opt(ctx, "repeat_spspps", "1");
      try_set_opt(ctx, "bf", "0");
    }

    if (avcodec_open2(ctx, codec, nullptr) < 0) {
      avcodec_free_context(&ctx);
      continue;
    }

    impl.codec = codec;
    impl.ctx = ctx;
    self->encoder_name_ = name;
    break;
  }

  if (!impl.ctx) {
    std::cerr << "VideoEncoder: no usable encoder for "
              << video_codec_format(params.codec) << " (tried:";
    for (const std::string& n : resolve_candidates(params)) std::cerr << " " << n;
    std::cerr << ")\n";
    return nullptr;
  }

  impl.frame = av_frame_alloc();
  impl.packet = av_packet_alloc();
  if (!impl.frame || !impl.packet) {
    std::cerr << "VideoEncoder: out of memory allocating frame/packet\n";
    return nullptr;
  }
  impl.frame->format = pix_fmt;
  impl.frame->width = params.width;
  impl.frame->height = params.height;
  if (av_frame_get_buffer(impl.frame, 0) < 0) {
    std::cerr << "VideoEncoder: could not allocate frame buffer\n";
    return nullptr;
  }

  if (params.codec == VideoCodec::H264) {
    impl.sws = sws_getContext(
      params.width, params.height, AV_PIX_FMT_BGR24,
      params.width, params.height, pix_fmt,
      SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!impl.sws) {
      std::cerr << "VideoEncoder: could not create BGR->YUV converter\n";
      return nullptr;
    }
  }

  std::cout << "  [video] " << video_codec_format(params.codec) << " encoder: "
            << self->encoder_name_ << " " << params.width << "x" << params.height
            << " gop=" << params.gop_size;
  if (params.lossless) {
    std::cout << " lossless";
  } else {
    std::cout << " " << params.bitrate_kbps << "kbps";
  }
  std::cout << "\n";

  return self;
}

std::optional<std::span<const std::byte>> VideoEncoder::encode(
  const cv::Mat& frame, bool& is_keyframe)
{
  Impl& impl = *impl_;
  is_keyframe = false;

  if (frame.cols != impl.ctx->width || frame.rows != impl.ctx->height) {
    std::cerr << "VideoEncoder: frame size " << frame.cols << "x" << frame.rows
              << " does not match encoder " << impl.ctx->width << "x" << impl.ctx->height << "\n";
    return std::nullopt;
  }

  if (av_frame_make_writable(impl.frame) < 0) {
    std::cerr << "VideoEncoder: frame not writable\n";
    return std::nullopt;
  }

  if (codec_ == VideoCodec::H264) {
    if (frame.type() != CV_8UC3) {
      std::cerr << "VideoEncoder: H264 expects CV_8UC3 (BGR), got type " << frame.type() << "\n";
      return std::nullopt;
    }
    cv::Mat contiguous = frame.isContinuous() ? frame : frame.clone();
    const uint8_t* src_slices[1] = {contiguous.data};
    const int src_stride[1] = {static_cast<int>(contiguous.step)};
    sws_scale(impl.sws, src_slices, src_stride, 0, impl.ctx->height,
              impl.frame->data, impl.frame->linesize);
  } else {
    if (frame.type() != CV_16UC1) {
      std::cerr << "VideoEncoder: H265 depth expects CV_16UC1, got type " << frame.type() << "\n";
      return std::nullopt;
    }
    // gray12le is 16-bit little-endian with only the low 12 bits used, so the
    // already-quantized codes copy straight in, row by row (strides differ).
    for (int y = 0; y < frame.rows; ++y) {
      std::memcpy(impl.frame->data[0] + static_cast<ptrdiff_t>(y) * impl.frame->linesize[0],
                  frame.ptr<uint16_t>(y), static_cast<size_t>(frame.cols) * sizeof(uint16_t));
    }
  }

  impl.frame->pts = impl.pts++;

  if (avcodec_send_frame(impl.ctx, impl.frame) < 0) {
    std::cerr << "VideoEncoder: send_frame failed\n";
    return std::nullopt;
  }

  // Zero-latency, no-B-frame configuration means one frame in yields one packet
  // out. Drain anyway and concatenate, so that an encoder which does emit two
  // NAL groups for a frame still produces a decodable single-frame message
  // rather than silently dropping data.
  impl.payload.clear();
  int ret = 0;
  while (ret >= 0) {
    ret = avcodec_receive_packet(impl.ctx, impl.packet);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
    if (ret < 0) {
      std::cerr << "VideoEncoder: receive_packet failed\n";
      return std::nullopt;
    }
    if (impl.packet->flags & AV_PKT_FLAG_KEY) is_keyframe = true;
    const auto* data = reinterpret_cast<const std::byte*>(impl.packet->data);
    impl.payload.insert(impl.payload.end(), data, data + impl.packet->size);
    av_packet_unref(impl.packet);
  }

  if (impl.payload.empty()) return std::nullopt;
  return std::span<const std::byte>(impl.payload.data(), impl.payload.size());
}

}  // namespace trossen::utils
