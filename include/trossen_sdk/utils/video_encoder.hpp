/**
 * @file video_encoder.hpp
 * @brief Per-frame H.264/H.265 encoder producing Annex B packets for MCAP.
 *
 * The TrossenMCAP backend can store camera streams as `foxglove.CompressedVideo`
 * instead of raw `foxglove.RawImage`. Raw BGR8 costs 921,600 bytes per VGA frame,
 * so a 3-camera 60 s episode lands at ~5 GB; the same content as H.264 is tens of
 * megabytes. This class is the encode half of that.
 *
 * It is deliberately a *one frame in, one packet out* interface rather than a
 * general muxing wrapper, because the recording path has two hard constraints:
 *
 *   1. Foxglove's CompressedVideo contract: each message must contain exactly
 *      enough NAL units to decode one frame, in Annex B form, with parameter sets
 *      repeated on every keyframe, and no B-frames (they need lookahead).
 *   2. The converter pairs camera frames to joint samples *by index*. An encoder
 *      that buffered or reordered frames would silently shift that alignment, so
 *      frame-in/packet-out must stay 1:1 and in order.
 *
 * Both are enforced by configuration here (zero-latency, no B-frames, no
 * lookahead) and checked at runtime by the caller.
 *
 * Available only when the SDK is built with TROSSEN_ENABLE_VIDEO_ENCODE; without
 * it, create() is still declared but the translation unit is not compiled, so
 * callers must gate on the macro.
 */

#ifndef TROSSEN_SDK__UTILS__VIDEO_ENCODER_HPP_
#define TROSSEN_SDK__UTILS__VIDEO_ENCODER_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

namespace trossen::utils {

/// @brief Which bitstream a VideoEncoder produces (the CompressedVideo `format`).
enum class VideoCodec {
  H264,  ///< 8-bit colour; the default for camera streams.
  H265,  ///< HEVC Main 12; used for 12-bit depth (`gray12le`).
};

/// @brief The CompressedVideo `format` string for a codec ("h264" / "h265").
const char* video_codec_format(VideoCodec codec);

/**
 * @brief Encodes successive frames into single-frame Annex B video packets.
 *
 * One instance per camera stream, living exactly as long as one episode: GOP
 * state is per-instance, so reusing an instance across episodes would emit a
 * non-keyframe first packet and leave the next episode undecodable from its
 * start.
 *
 * Not thread-safe. The MCAP backend calls encode() under its writer mutex.
 */
class VideoEncoder {
public:
  /// @brief Encoder construction parameters.
  struct Params {
    int width{0};
    int height{0};
    /// @brief Nominal frame rate, used for the stream time base only.
    int fps{30};
    /// @brief Target bitrate; ignored when `lossless` is set.
    int bitrate_kbps{6000};
    /**
     * @brief Keyframe interval in frames.
     *
     * Also the random-access cost downstream: LeRobot training reads single
     * frames by timestamp and pays a decode of the enclosing GOP, so a large
     * value here makes training reads proportionally more expensive. lerobot's
     * own default is 2. Keep this small.
     */
    int gop_size{10};
    VideoCodec codec{VideoCodec::H264};
    /**
     * @brief Preferred encoder name, or "auto" to probe hardware then software.
     *
     * Accepted: "auto", "nvenc", "vaapi", "x264"/"x265", or a literal
     * libavcodec encoder name (e.g. "h264_nvenc").
     */
    std::string encoder{"auto"};
    /// @brief Lossless mode (depth): disables rate control, requires H265.
    bool lossless{false};
  };

  /**
   * @brief Build an encoder, resolving and opening the underlying codec.
   *
   * @param params Encoder parameters; `width`/`height` must be non-zero.
   * @return An open encoder, or nullptr when no usable encoder was found or
   *         the parameters were rejected (reason logged to stderr).
   */
  static std::unique_ptr<VideoEncoder> create(const Params& params);

  ~VideoEncoder();

  VideoEncoder(const VideoEncoder&) = delete;
  VideoEncoder& operator=(const VideoEncoder&) = delete;

  /**
   * @brief Encode one frame and return its Annex B packet.
   *
   * The frame must match the configured size, and be CV_8UC3 (BGR) for H264 or
   * CV_16UC1 (already-quantized 12-bit codes) for lossless H265.
   *
   * @param frame Input image.
   * @param[out] is_keyframe Set to true when the returned packet is a keyframe.
   * @return A view of the packet, valid until the next encode() call, or
   *         std::nullopt if the encoder produced no packet for this frame
   *         (which breaks the 1:1 invariant and is logged by the caller).
   */
  std::optional<std::span<const std::byte>> encode(const cv::Mat& frame, bool& is_keyframe);

  /// @brief Resolved libavcodec encoder name, e.g. "h264_nvenc".
  const std::string& encoder_name() const { return encoder_name_; }

  /// @brief The codec this encoder emits.
  VideoCodec codec() const { return codec_; }

private:
  VideoEncoder();

  /// @brief Opaque libavcodec state, kept out of this header.
  struct Impl;
  std::unique_ptr<Impl> impl_;

  std::string encoder_name_;
  VideoCodec codec_{VideoCodec::H264};
};

}  // namespace trossen::utils

#endif  // TROSSEN_SDK__UTILS__VIDEO_ENCODER_HPP_
