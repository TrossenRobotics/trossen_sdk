/**
 * @file video_encoder.cpp
 * @brief libavcodec implementation of the per-frame Annex B video encoder.
 */

#include "trossen_sdk/utils/video_encoder.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace trossen::utils {
namespace {

/**
 * @brief Hardware-first candidate encoder names to try for a codec, in order.
 *
 * @param codec Which bitstream is being produced.
 * @return An ordered list of libavcodec encoder names: hardware options first, `libx264`/`libx265`
 * last as the software fallback.
 */
const std::vector<std::string>& probe_order(VideoCodec codec) {
  static const std::vector<std::string> h264_candidates{"h264_nvenc", "h264_vaapi", "libx264"};
  static const std::vector<std::string> h265_candidates{"hevc_nvenc", "hevc_vaapi", "libx265"};

  switch (codec) {
    case VideoCodec::H264:
      return h264_candidates;
    case VideoCodec::H265:
      return h265_candidates;
  }
  // Unreachable: every VideoCodec enumerator is handled above.
  static const std::vector<std::string> empty_candidates;
  return empty_candidates;
}

/**
 * @brief Expand `Params::encoder` into the ordered list of libavcodec encoder names to try.
 *
 * @param params Encoder parameters; `params.encoder` is the alias to resolve ("auto", "nvenc",
 * "vaapi", "x264"/"x265", or a literal libavcodec encoder name).
 * @return One or more candidate encoder names, in the order `create()` should try them.
 */
std::vector<std::string> resolve_candidates(const VideoEncoder::Params& params) {
  const std::string& requested = params.encoder;

  if (requested == "auto") {
    return probe_order(params.codec);
  }

  const bool is_h264 = params.codec == VideoCodec::H264;

  if (requested == "nvenc") {
    return {is_h264 ? "h264_nvenc" : "hevc_nvenc"};
  }
  if (requested == "vaapi") {
    return {is_h264 ? "h264_vaapi" : "hevc_vaapi"};
  }
  if (requested == "x264" || requested == "x265") {
    return {is_h264 ? "libx264" : "libx265"};
  }

  // Anything else is taken as a literal libavcodec encoder name.
  return {requested};
}

/**
 * @brief Set one codec-specific option, warning (not failing) if it's rejected.
 *
 * @param ctx Codec context to configure; mutated in place.
 * @param key Option name (e.g. "preset", "tune", "crf").
 * @param value Option value.
 * @return true if the encoder accepted the option, false if it was rejected (a warning is logged
 * to stderr on rejection).
 */
bool try_set_opt(AVCodecContext* ctx, const char* key, const char* value) {
  const int result = av_opt_set(ctx->priv_data, key, value, 0);
  if (result < 0) {
    std::cerr << "warning: encoder option '" << key << "=" << value << "' was rejected\n";
    return false;
  }
  return true;
}

}  // namespace

const char* video_codec_format(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::H264:
      return "h264";
    case VideoCodec::H265:
      return "h265";
  }
  // Unreachable: every VideoCodec enumerator is handled above.
  return "";
}

/**
 * @brief Owns every libavcodec/libavutil/libswscale resource for one open encoder.
 *
 * Kept out of the public header (PIMPL) so consumers of VideoEncoder never need FFmpeg's headers
 * on their include path.
 */
struct VideoEncoder::Impl {
  const AVCodec* codec = nullptr;  ///< Static library-owned descriptor; borrowed, never freed.
  AVCodecContext* ctx = nullptr;   ///< Open encoder state, allocated against `codec`.
  AVFrame* frame = nullptr;        ///< Reusable frame buffer passed to avcodec_send_frame().
  AVPacket* packet = nullptr;      ///< Reusable packet buffer filled by avcodec_receive_packet().
  SwsContext* sws = nullptr;       ///< BGR->YUV420P converter for H264; unused (nullptr) for H265.
  int64_t pts = 0;                 ///< Monotonically increasing presentation timestamp.
  std::string encoder_name;        ///< Resolved libavcodec encoder name (e.g. "libx264").
  VideoCodec video_codec = VideoCodec::H264;  ///< Which codec this encoder was opened for.

  /**
   * @brief Frees every owned FFmpeg resource, in reverse order of allocation.
   *
   * `frame`, `packet`, and `sws` don't structurally depend on `ctx` (each owns only its own
   * memory, and every FFmpeg `_free` call below is safe on an already-null pointer), so this
   * order isn't required for correctness. It follows RAII convention (destroy in the reverse
   * of construction order) rather than a real ownership dependency. `codec` is never freed: it's
   * a borrowed pointer into libavcodec's own static registry, not something we allocated.
   */
  ~Impl() {
    sws_freeContext(sws);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
  }
};

/// @brief Takes ownership of an already-built Impl (constructed by create()).
VideoEncoder::VideoEncoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

/// @brief Defaulted here (not in the header) so ~Impl() sees Impl's complete definition.
VideoEncoder::~VideoEncoder() = default;

std::unique_ptr<VideoEncoder> VideoEncoder::create(const Params& params) {
  if (params.width <= 0 || params.height <= 0) {
    std::cerr << "VideoEncoder::create: width/height must be positive\n";
    return nullptr;
  }
  if (params.lossless && params.codec != VideoCodec::H265) {
    std::cerr << "VideoEncoder::create: lossless mode requires H265\n";
    return nullptr;
  }
  if (params.codec == VideoCodec::H264 && (params.width % 2 != 0 || params.height % 2 != 0)) {
    std::cerr << "VideoEncoder::create: H264 requires even width/height\n";
    return nullptr;
  }

  // AVPixelFormat is libavutil's enum naming every raw pixel memory layout FFmpeg understands:
  // how many planes there are, what each plane stores, what bit depth, byte order, etc. It has
  // nothing to do with compression yet; it just describes the *uncompressed* frame data we're
  // about to hand the encoder, so the encoder knows how to read it.
  //
  // AV_PIX_FMT_YUV420P ("4:2:0" chroma subsampling) is the standard 8-bit color layout for H264:
  // three separate planes, Y (luma/brightness, full resolution), U and V (chroma/color, each
  // stored at HALF resolution in both width and height, i.e. one U and one V sample cover a 2x2
  // block of Y samples). This works because human vision is far more sensitive to brightness
  // detail than color detail, so throwing away 3/4 of the color samples is nearly invisible while
  // cutting the raw data size roughly in half before compression even starts. This is also why
  // H264 frames need even width/height (validated above): a 2x2 subsampling block can't be formed
  // from an odd dimension.
  //
  // AV_PIX_FMT_GRAY12LE is one plane, no subsampling, 12 bits per sample, little-endian byte
  // order. Depth has no "color" to subsample: each pixel is a single quantized 12-bit code from
  // depth_quantization.hpp, so this format stores every value at full resolution and full
  // precision, which lossless HEVC12 encoding then preserves exactly.
  const AVPixelFormat pix_fmt =
      params.codec == VideoCodec::H264 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_GRAY12LE;

  // These two will hold the encoder we actually end up opening, once the loop below finds one
  // that works. They start null; if the loop never succeeds, they stay null, and we bail out below.
  const AVCodec* codec = nullptr;
  AVCodecContext* ctx = nullptr;

  // Try every candidate encoder name in order (hardware first, then software, per
  // resolve_candidates()/probe_order()), and keep the first one that successfully opens.
  for (const std::string& name : resolve_candidates(params)) {
    // AVCodec is libavcodec's read-only descriptor for one named encoder implementation (e.g.
    // "libx264" or "h264_nvenc"). avcodec_find_encoder_by_name() looks it up in libavcodec's own
    // built-in registry by name; it returns nullptr if this build of FFmpeg wasn't compiled with
    // that particular encoder (e.g. no NVIDIA driver support compiled in), which is exactly the
    // "not available on this machine" case we want to skip past, not fail on.
    const AVCodec* candidate_codec = avcodec_find_encoder_by_name(name.c_str());
    if (candidate_codec == nullptr) {
      continue;
    }

    // AVCodecContext is the actual encoder INSTANCE: all of its configuration (resolution,
    // bitrate, tuning options, internal state once opened) lives here. avcodec_alloc_context3()
    // allocates one pre-sized/defaulted for this specific codec.
    AVCodecContext* candidate_ctx = avcodec_alloc_context3(candidate_codec);
    if (candidate_ctx == nullptr) {
      continue;
    }

    // Settings every encoder needs, regardless of family:
    candidate_ctx->width = params.width;
    candidate_ctx->height = params.height;
    // pix_fmt isn't a Params field: it's a strict, always-1:1 consequence of codec (H264 always
    // means YUV420P color, H265 always means GRAY12LE depth), not an independent policy choice
    // like depth_quantization's range/log parameters. Exposing it would only let callers build
    // invalid combinations this SDK never actually uses.
    candidate_ctx->pix_fmt = pix_fmt;
    // time_base is the "clock tick" every timestamp on this stream is measured in, expressed as a
    // fraction of a second (AVRational = a rational number: numerator, denominator). {1, fps}
    // means one tick = 1/fps seconds, i.e. one tick per frame at our nominal frame rate.
    candidate_ctx->time_base = AVRational{1, params.fps};
    // framerate is the *nominal* frame rate metadata (informational/muxing hint), as a plain
    // fraction fps/1, distinct from time_base, which defines the timestamp units.
    // It's an AVRational (not a plain int) because the field itself is typed that way. Exact
    // integer ratios avoid the rounding error a float would accumulate over many frames, and they
    // can represent non-integer rates (e.g. NTSC's 30000/1001) that no single int ever could.
    candidate_ctx->framerate = AVRational{params.fps, 1};
    candidate_ctx->gop_size = params.gop_size;
    // The CompressedVideo schema requires each message to decode as a single standalone image, so
    // B-frames (which predict from both past and future frames) aren't allowed. Capping this at 0
    // is the actual mechanism that enforces it, not just an encoder preset. It also keeps every
    // input frame mapped to exactly one output packet, which the recorder's frame:joint-state
    // index pairing depends on.
    candidate_ctx->max_b_frames = 0;
    // Lossless mode disables ordinary rate control entirely (see the per-family "lossless=1"
    // option below), so setting a target bitrate here would be meaningless/ignored.
    if (!params.lossless) {
      candidate_ctx->bit_rate = static_cast<int64_t>(params.bitrate_kbps) * 1000;
    }

    // Per-encoder-family tuning. These are extra options specific to each encoder implementation
    // (libavcodec exposes them as generic key/value "AVOptions" rather than typed struct fields,
    // which is what try_set_opt()/av_opt_set() is for), matched by checking for each family's name
    // as a substring of the candidate encoder name.
    if (name.find("libx264") != std::string::npos) {
      // "ultrafast": spend the least CPU time searching for compression gains (speed over ratio).
      // "zerolatency": disables internal frame buffering/lookahead, so encode() gets a packet back
      // immediately for every frame sent in, instead of the encoder holding frames to reorder.
      try_set_opt(candidate_ctx, "preset", "ultrafast");
      try_set_opt(candidate_ctx, "tune", "zerolatency");
    } else if (name.find("libx265") != std::string::npos) {
      try_set_opt(candidate_ctx, "preset", "ultrafast");
      try_set_opt(candidate_ctx, "tune", "zerolatency");
      if (params.lossless) {
        // libx265's own lossless switch, passed through its "x265-params" option string. This is
        // what actually makes HEVC12 depth encoding lossless. Ordinary H265 encoding is lossy
        // by default even at very high quality settings.
        try_set_opt(candidate_ctx, "x265-params", "lossless=1");
      }
    } else if (name.find("nvenc") != std::string::npos) {
      // nvenc (NVIDIA's hardware encoder) uses its own preset/tune vocabulary, not libx264/265's:
      // "p1" = fastest hardware preset, "ll" = low-latency tuning, "zerolatency" = an explicit
      // nvenc-specific boolean option for the same "don't buffer frames" behavior as above.
      try_set_opt(candidate_ctx, "preset", "p1");
      try_set_opt(candidate_ctx, "tune", "ll");
      try_set_opt(candidate_ctx, "zerolatency", "1");
    }
    // vaapi candidates are left unconfigured here: real hardware use needs an AVHWDeviceContext /
    // AVHWFramesContext wired up first, which is out of scope for now. Left as-is,
    // avcodec_open2() below simply fails without one, and the loop already treats any open
    // failure as "try the next candidate", so it falls through to software cleanly, for free.

    // avcodec_open2() actually initializes the encoder with everything configured above:
    // allocating its internal working buffers, validating the options we set are legal for this
    // specific codec, etc. It returns a negative AVERROR code on failure (invalid combination of
    // options, unsupported resolution, hardware truly unavailable, ...); 0 or positive means
    // success. If it fails, free this attempt's context and move on to the next candidate name.
    if (avcodec_open2(candidate_ctx, candidate_codec, nullptr) < 0) {
      avcodec_free_context(&candidate_ctx);
      continue;
    }

    // This candidate opened successfully, so keep it and stop probing.
    codec = candidate_codec;
    ctx = candidate_ctx;
    break;
  }

  // If nothing in the whole candidate list opened, there's no encoder to return.
  if (ctx == nullptr) {
    std::cerr << "VideoEncoder::create: no usable encoder found for requested codec/encoder\n";
    return nullptr;
  }

  // Allocate the reusable frame/packet buffers encode() will fill and drain on every call, rather
  // than allocating fresh ones per frame.
  AVFrame* frame = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();
  SwsContext* sws = nullptr;

  // A single cleanup path for every failure below, so the three av_*_free calls aren't repeated
  // at each failure site. Safe to call unconditionally: every FFmpeg _free function here is a
  // no-op on a null pointer.
  auto fail = [&](const char* message) {
    std::cerr << "VideoEncoder::create: " << message << '\n';
    // Frees the BGR-to-YUV420P converter (libswscale).
    sws_freeContext(sws);
    // Frees the packet buffer and nulls the pointer (libavcodec).
    av_packet_free(&packet);
    // Frees the frame buffer and nulls the pointer (libavcodec).
    av_frame_free(&frame);
    // Closes and frees the encoder context, nulls the pointer.
    avcodec_free_context(&ctx);
  };

  if (frame == nullptr || packet == nullptr) {
    fail("failed to allocate frame or packet");
    return nullptr;
  }

  // frame needs to know its own format/dimensions before av_frame_get_buffer() can size and
  // allocate its pixel data buffers.
  frame->format = pix_fmt;
  frame->width = params.width;
  frame->height = params.height;
  // The trailing 0 is the buffer alignment; 0 means "let FFmpeg pick a sensible default" rather
  // than us hand-tuning it.
  if (av_frame_get_buffer(frame, 0) < 0) {
    fail("failed to allocate frame pixel buffer");
    return nullptr;
  }

  // Only H264 color needs a converter: incoming frames arrive as raw BGR (one interleaved plane,
  // 8 bits/channel) and must be converted into YUV420P before encoding. H265 depth frames are
  // already gray12le, so encode() will copy the quantized codes straight into the frame's single
  // plane. No conversion is needed, so sws stays null.
  if (params.codec == VideoCodec::H264) {
    sws = sws_getContext(params.width, params.height, AV_PIX_FMT_BGR24, params.width,
                          params.height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr,
                          nullptr);
    if (sws == nullptr) {
      fail("failed to create BGR-to-YUV420P converter");
      return nullptr;
    }
  }

  // Everything succeeded: hand ownership of every allocated resource to a new Impl, then wrap
  // that in a VideoEncoder. std::make_unique can't be used here since VideoEncoder's constructor
  // is private. However, create() is itself a member function of VideoEncoder, so it can call
  // `new VideoEncoder(...)` directly despite that.
  auto impl = std::make_unique<Impl>();
  impl->codec = codec;
  impl->ctx = ctx;
  impl->frame = frame;
  impl->packet = packet;
  impl->sws = sws;
  impl->encoder_name = codec->name;
  impl->video_codec = params.codec;

  std::cerr << "VideoEncoder::create: opened '" << codec->name << "' (" << params.width << "x"
             << params.height << " @ " << params.fps << " fps)\n";

  return std::unique_ptr<VideoEncoder>(new VideoEncoder(std::move(impl)));
}

const std::string& VideoEncoder::encoder_name() const { return impl_->encoder_name; }

VideoCodec VideoEncoder::codec() const { return impl_->video_codec; }

}  // namespace trossen::utils
