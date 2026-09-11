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
// How camera frames are stored: "raw" (foxglove.RawImage) or "video" (foxglove.CompressedVideo)
inline constexpr char TROSSEN_MCAP_IMAGE_ENCODING_RAW[] = "raw";
inline constexpr char TROSSEN_MCAP_IMAGE_ENCODING_VIDEO[] = "video";
inline constexpr char TROSSEN_MCAP_DEFAULT_IMAGE_ENCODING[] = "raw";
inline constexpr int TROSSEN_MCAP_DEFAULT_VIDEO_BITRATE_KBPS = 6000;

// Keyframe interval; also the downstream random-access cost
inline constexpr int TROSSEN_MCAP_DEFAULT_VIDEO_KEYFRAME_INTERVAL = 10;
inline constexpr char TROSSEN_MCAP_DEFAULT_VIDEO_ENCODER[] = "auto";

// Nominal frame rate for the encoder's stream time base only
inline constexpr int TROSSEN_MCAP_VIDEO_NOMINAL_FPS = 30;

struct TrossenMCAPBackendConfig : public BaseConfig {
  std::string root{trossen::io::backends::get_default_root_path().string()};
  std::string robot_name{trossen::io::backends::DEFAULT_ROBOT_NAME};
  int chunk_size_bytes{TROSSEN_MCAP_DEFAULT_CHUNK_SIZE_BYTES};
  std::string compression{TROSSEN_MCAP_DEFAULT_COMPRESSION};
  std::string dataset_id{trossen::io::backends::auto_generate_dataset_id()};

  /// Text description of the task being demonstrated (e.g. "pick up the cube").
  /// Written to the MCAP file-level metadata if non-empty.
  std::string task_description{""};

  /// How camera frames are stored: "raw" or "video".
  /// "video" requires the SDK to be built with TROSSEN_ENABLE_VIDEO_ENCODE.
  std::string image_encoding{TROSSEN_MCAP_DEFAULT_IMAGE_ENCODING};

  /// Target bitrate for color video; ignored for depth (always lossless).
  int video_bitrate_kbps{TROSSEN_MCAP_DEFAULT_VIDEO_BITRATE_KBPS};
  int video_keyframe_interval{TROSSEN_MCAP_DEFAULT_VIDEO_KEYFRAME_INTERVAL};

  /// Encoder preference: "auto", "nvenc", "vaapi", "x264"/"x265", or a literal name.
  std::string video_encoder{TROSSEN_MCAP_DEFAULT_VIDEO_ENCODER};

  /// True when camera frames should be stored as compressed video.
  bool records_video() const { return image_encoding == TROSSEN_MCAP_IMAGE_ENCODING_VIDEO; }

  /// True when `image_encoding` names a format the backend understands.
  bool image_encoding_is_valid() const {
    return image_encoding == TROSSEN_MCAP_IMAGE_ENCODING_RAW ||
           image_encoding == TROSSEN_MCAP_IMAGE_ENCODING_VIDEO;
  }

  std::string type() const override { return "trossen_mcap_backend"; }

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
    if (j.contains("task_description")) j.at("task_description").get_to(c.task_description);
    if (j.contains("image_encoding")) j.at("image_encoding").get_to(c.image_encoding);
    if (j.contains("video_bitrate_kbps")) j.at("video_bitrate_kbps").get_to(c.video_bitrate_kbps);
    if (j.contains("video_keyframe_interval"))
      j.at("video_keyframe_interval").get_to(c.video_keyframe_interval);
    if (j.contains("video_encoder")) j.at("video_encoder").get_to(c.video_encoder);

    return c;
  }
};

REGISTER_CONFIG(TrossenMCAPBackendConfig, "trossen_mcap_backend");

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__BACKENDS__TROSSEN_MCAP_BACKEND_CONFIG_HPP_
