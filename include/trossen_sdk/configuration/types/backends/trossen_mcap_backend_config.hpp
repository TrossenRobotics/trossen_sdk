/**
 * @file trossen_mcap_backend_config.hpp
 * @brief Configuration for TrossenMCAP backend
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__TROSSEN_MCAP_BACKEND_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__TROSSEN_MCAP_BACKEND_CONFIG_HPP_

#include "trossen_sdk/configuration/base_config.hpp"
#include "trossen_sdk/configuration/config_registry.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/io/backend_utils.hpp"

namespace trossen::configuration {

// TrossenMCAP backend specific constants
inline constexpr int TROSSEN_MCAP_DEFAULT_CHUNK_SIZE_BYTES = 4 * 1024 * 1024;
inline constexpr char TROSSEN_MCAP_DEFAULT_COMPRESSION[] = "";

// How camera frames are stored: "raw" writes foxglove.RawImage (uncompressed
// pixels), "video" writes foxglove.CompressedVideo (H.264 colour, lossless
// HEVC Main 12 depth). Raw remains the default so existing rigs keep their
// current output until video recording has been verified on hardware.
inline constexpr char TROSSEN_MCAP_IMAGE_ENCODING_RAW[] = "raw";
inline constexpr char TROSSEN_MCAP_IMAGE_ENCODING_VIDEO[] = "video";
inline constexpr char TROSSEN_MCAP_DEFAULT_IMAGE_ENCODING[] = "raw";

inline constexpr int TROSSEN_MCAP_DEFAULT_VIDEO_BITRATE_KBPS = 6000;
// Keyframe interval. Also the downstream random-access cost: LeRobot training
// reads single frames by timestamp and decodes the enclosing GOP to get one,
// and the offline converter stream-copies this bitstream rather than
// re-encoding it, so whatever is chosen here is what training pays. lerobot's
// own encoder default is 2; keep this small.
inline constexpr int TROSSEN_MCAP_DEFAULT_VIDEO_KEYFRAME_INTERVAL = 10;
inline constexpr char TROSSEN_MCAP_DEFAULT_VIDEO_ENCODER[] = "auto";
// Frame rate handed to the encoder for its stream time base only. True frame
// timing comes from each message's MCAP log time (the capture timestamp), which
// is what the converter aligns on, so this value does not need to match the
// camera's actual rate.
inline constexpr int TROSSEN_MCAP_VIDEO_NOMINAL_FPS = 30;

struct TrossenMCAPBackendConfig : public BaseConfig {
  std::string root{trossen::io::backends::get_default_root_path().string()};
  std::string robot_name{trossen::io::backends::DEFAULT_ROBOT_NAME};
  int chunk_size_bytes{TROSSEN_MCAP_DEFAULT_CHUNK_SIZE_BYTES};
  std::string compression{TROSSEN_MCAP_DEFAULT_COMPRESSION};
  std::string dataset_id{trossen::io::backends::auto_generate_dataset_id()};
  // TODO(shantanuparab-tr): Remove episode index if not being used
  int episode_index{0};
  // Natural-language task prompt embedded into every episode's MCAP metadata
  // (the LeRobot `task`). Mutable at runtime between episodes: the backend
  // re-reads this at each open(), and because the backend caches the same
  // shared_ptr the GlobalConfig holds, changing it there (e.g. from Python via
  // GlobalConfig::get("trossen_mcap_backend")) changes what the next episode
  // records. Empty = no task embedded (converter falls back to its task_name).
  std::string task{};

  // ── Camera storage format ──
  //
  // "raw" | "video"; see the constants above. "video" requires the SDK to be
  // built with TROSSEN_ENABLE_VIDEO_ENCODE. The backend fails the episode at
  // open() when it cannot honour the request, rather than silently falling back
  // to raw and producing a dataset nobody asked for.
  std::string image_encoding{TROSSEN_MCAP_DEFAULT_IMAGE_ENCODING};
  // Per colour camera. Depth ignores it: depth is already quantized to 12 bits,
  // so it is encoded losslessly and further loss would not be recoverable.
  int video_bitrate_kbps{TROSSEN_MCAP_DEFAULT_VIDEO_BITRATE_KBPS};
  int video_keyframe_interval{TROSSEN_MCAP_DEFAULT_VIDEO_KEYFRAME_INTERVAL};
  // "auto" probes hardware then software. Also accepts "nvenc", "vaapi",
  // "x264"/"x265", or a literal libavcodec encoder name.
  std::string video_encoder{TROSSEN_MCAP_DEFAULT_VIDEO_ENCODER};

  std::string type() const override { return "trossen_mcap_backend"; }

  /// @brief True when camera frames should be stored as compressed video.
  bool records_video() const { return image_encoding == TROSSEN_MCAP_IMAGE_ENCODING_VIDEO; }

  /// @brief True when `image_encoding` names a format the backend understands.
  bool image_encoding_is_valid() const {
    return image_encoding == TROSSEN_MCAP_IMAGE_ENCODING_RAW ||
           image_encoding == TROSSEN_MCAP_IMAGE_ENCODING_VIDEO;
  }

  static TrossenMCAPBackendConfig from_json(const nlohmann::json& j) {
    TrossenMCAPBackendConfig c;

    // Only override if present in JSON
    if (j.contains("root")) {
      std::string raw_root;
      j.at("root").get_to(raw_root);
      c.root = trossen::io::backends::expand_user(raw_root).string();
    }
    if (j.contains("robot_name")) j.at("robot_name").get_to(c.robot_name);
    if (j.contains("chunk_size_bytes")) j.at("chunk_size_bytes").get_to(c.chunk_size_bytes);
    if (j.contains("compression")) j.at("compression").get_to(c.compression);
    if (j.contains("dataset_id")) j.at("dataset_id").get_to(c.dataset_id);
    if (j.contains("episode_index")) j.at("episode_index").get_to(c.episode_index);
    if (j.contains("task")) j.at("task").get_to(c.task);
    if (j.contains("image_encoding")) j.at("image_encoding").get_to(c.image_encoding);
    if (j.contains("video_bitrate_kbps")) {
      j.at("video_bitrate_kbps").get_to(c.video_bitrate_kbps);
    }
    if (j.contains("video_keyframe_interval")) {
      j.at("video_keyframe_interval").get_to(c.video_keyframe_interval);
    }
    if (j.contains("video_encoder")) j.at("video_encoder").get_to(c.video_encoder);

    return c;
  }
};

REGISTER_CONFIG(TrossenMCAPBackendConfig, "trossen_mcap_backend");

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__TROSSEN_MCAP_BACKEND_CONFIG_HPP_
