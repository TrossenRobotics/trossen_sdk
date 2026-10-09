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
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace trossen::utils {
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

const std::string& VideoEncoder::encoder_name() const { return impl_->encoder_name; }

VideoCodec VideoEncoder::codec() const { return impl_->video_codec; }

}  // namespace trossen::utils
