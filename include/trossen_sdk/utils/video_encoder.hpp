#ifndef TROSSEN_SDK__UTILS__VIDEO_ENCODER_HPP_
#define TROSSEN_SDK__UTILS__VIDEO_ENCODER_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace trossen::utils {

/// @brief Which bitstream a VideoEncoder produces (the CompressedVideo
/// `format`).
enum class VideoCodec {
  H264,  ///< 8-bit color; the default for camera streams.
  H265,  ///< HEVC Main 12; used for 12-bit depth (`gray12le`).
};

/// @brief The CompressedVideo `format` string for a codec ("h264" / "h265").
const char* video_codec_format(VideoCodec codec);

class VideoEncoder {
 public:
  /// @brief Encoder construction parameters.
  struct Params {
    /// @brief Frame width in pixels. Every frame passed to encode() must match.
    int width{0};
    /// @brief Frame height in pixels. Every frame passed to encode() must match.
    int height{0};
    /// @brief Nominal frame rate, used for the stream time base only.
    int fps{30};
    /// @brief Target bitrate; ignored when `lossless` is set.
    int bitrate_kbps{6000};
    /**
     * @brief Keyframe interval in frames.
     * Also, the random-access cost downstream: LeRobot training reads single frames by timestamp
     * and pays the cost of decoding the enclosing GOP (Group of Pictures), so a large value here
     * makes training read proportionally more expensive. LeRobot's own default is 2. Keep this
     * small.
     */
    int gop_size{10};
    /// @brief Which bitstream to produce: H264 for color, H265 for lossless depth.
    VideoCodec codec{VideoCodec::H264};
    /**
     * @brief Preferred encoder name, or "auto" to probe hardware then software.
     * Accepted: "auto", "nvenc", "vaapi", "x264"/"x265", or a literal libavcodec encoder name
     * (e.g. "h264_nvenc").
     */
    std::string encoder{"auto"};
    /// @brief Lossless mode (depth): disables rate control, requires H265.
    bool lossless{false};
  };

  /**
   * @brief Build an encoder, resolving and opening the underlying codec.
   *
   * @param params Encoder parameters; width/height must be non-zero.
   * @return An open encoder, or nullptr when no usable encoder was found or the parameters were
   * rejected (reason logged to stderr).
   */
  static std::unique_ptr<VideoEncoder> create(const Params& params);

  /// @brief One encoded frame's output: the Annex B bytes, and whether they form a keyframe.
  struct EncodedFrame {
    std::vector<std::byte> data;
    bool is_keyframe{false};
  };

  /**
   * @brief Encode one raw frame.
   *
   * @param data Raw pixel bytes: BGR8 for H264, packed 12-bit depth codes for H265.
   * @param size Byte length of `data`; must match width*height*bytes-per-pixel for this encoder.
   * @return The encoded bytes for this frame (empty on failure, reason logged to stderr).
   */
  EncodedFrame encode(const uint8_t* data, size_t size);

  /// @brief Name of the libavcodec encoder actually opened (e.g. "libx264", "h264_nvenc").
  const std::string& encoder_name() const;

  /// @brief Which codec this encoder was built for.
  VideoCodec codec() const;

  ~VideoEncoder();

 private:
  struct Impl;

  explicit VideoEncoder(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace trossen::utils

#endif  // TROSSEN_SDK__UTILS__VIDEO_ENCODER_HPP_
