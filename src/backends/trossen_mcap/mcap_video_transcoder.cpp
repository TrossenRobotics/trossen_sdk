/**
 * @file mcap_video_transcoder.cpp
 * @brief Implementation of transcode_images_to_video().
 *
 * Reads with the MCAP library (the reader implementation lives in mcap_dataset_loader.cpp,
 * so this unit must not define MCAP_IMPLEMENTATION) and writes with the Foxglove SDK
 * writer, the same one the recorder uses, with the recorder's MCAP profile.
 */

#include "trossen_sdk/io/backends/trossen_mcap/mcap_video_transcoder.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <opencv2/opencv.hpp>

#include "foxglove/channel.hpp"
#include "foxglove/error.hpp"
#include "foxglove/mcap.hpp"
#include "foxglove/schemas.hpp"
#include "mcap/reader.hpp"
#include "nlohmann/json.hpp"

#include "RawImage.pb.h"
#include "trossen_sdk/configuration/types/backends/trossen_mcap_backend_config.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"
#include "trossen_sdk/utils/remove_on_exit.hpp"
#include "trossen_sdk/utils/video_encoder.hpp"

namespace fs = std::filesystem;

namespace trossen::io::backends {

namespace {

/// @brief Size the encode buffer starts at; grown once if a message needs more.
constexpr size_t kInitialEncodedBufferSize = 4 * 1024 * 1024;

/**
 * @brief mcap reader callback: log a recoverable parsing issue and keep reading.
 *
 * @param problem The issue the reader reported.
 */
void on_problem(const mcap::Status& problem) {
  std::cerr << "Warning: MCAP parsing issue: " << problem.message << "\n";
}

/// @brief A camera as the recording's dataset_info describes it.
struct DescribedCamera {
  /// @brief Camera name in dataset_info.
  std::string name;
  /// @brief Whether the camera records a depth map.
  bool depth{false};
  /// @brief Whether the camera's frames are stored as `foxglove.RawImage`.
  bool raw{false};
};

/**
 * @brief Read and parse the metadata record that `index` points at.
 *
 * @param reader Open reader on the recording.
 * @param index Index entry of the record to read.
 * @param record Set to the parsed record.
 * @return false when it cannot be read (reason logged to stderr).
 */
bool read_metadata(
  mcap::McapReader& reader, const mcap::MetadataIndex& index, mcap::Metadata& record) {
  mcap::Record raw_record;
  auto status = mcap::McapReader::ReadRecord(*reader.dataSource(), index.offset, &raw_record);
  if (status.ok()) status = mcap::McapReader::ParseMetadata(raw_record, &record);
  if (!status.ok()) {
    std::cerr << "Error: Failed to read metadata record '" << index.name
              << "': " << status.message << "\n";
    return false;
  }
  return true;
}

/**
 * @brief Read the recording's metadata record.
 *
 * @param reader Open reader on the recording.
 * @param fields Set to the record's keys and values; left empty when there is no record.
 * @return false when the record exists but cannot be read (reason logged to stderr).
 */
bool read_recording_metadata(mcap::McapReader& reader, mcap::KeyValueMap& fields) {
  const auto& indexes = reader.metadataIndexes();
  const auto index = indexes.find(trossen_mcap_defs::kRecordingMetadataName);
  if (index == indexes.end()) return true;
  mcap::Metadata record;
  if (!read_metadata(reader, index->second, record)) return false;
  fields = std::move(record.metadata);
  return true;
}

/**
 * @brief One key of a metadata record.
 *
 * @param fields The record's keys and values.
 * @param key Key to look up.
 * @return The key's value, or an empty string when it is absent.
 */
std::string field_or_empty(const mcap::KeyValueMap& fields, const char* key) {
  const auto it = fields.find(key);
  return it == fields.end() ? std::string() : it->second;
}

/**
 * @brief Read each camera's topic, depth flag and storage from the recording's dataset_info.
 *
 * @param dataset_info The recording's dataset_info JSON.
 * @param cameras Set to the cameras keyed by topic.
 * @return False, with a message on stderr, if dataset_info does not parse or a camera has no
 *   topic, boolean is_depth_map and storage.
 */
bool read_described_cameras(const std::string& dataset_info,
                            std::map<std::string, DescribedCamera>& cameras) {
  const auto info = nlohmann::json::parse(dataset_info, nullptr, /*allow_exceptions=*/false);
  if (info.is_discarded()) {
    std::cerr << "Error: the recording's dataset_info does not parse\n";
    return false;
  }
  const auto described = info.value("cameras", nlohmann::json::object());
  for (const auto& [name, camera] : described.items()) {
    const auto topic = camera.value("topic", nlohmann::json());
    const auto depth = camera.value("is_depth_map", nlohmann::json());
    const auto storage = camera.value("storage", nlohmann::json());
    if (!topic.is_string() || !depth.is_boolean() || !storage.is_string()) {
      std::cerr << "Error: camera '" << name << "' has no dataset_info topic, is_depth_map and "
                << "storage\n";
      return false;
    }
    cameras[topic.get<std::string>()] =
      DescribedCamera{name, depth.get<bool>(), storage == trossen_mcap_defs::kRawStorage};
  }
  return true;
}

/// @brief Map a RawImage `encoding` string to the OpenCV type of its pixel buffer.
/// @return The CV type, or -1 when the encoding is not one this tool can re-encode.
int cv_type_for_encoding(const std::string& encoding) {
  if (encoding == "bgr8" || encoding == "rgb8" || encoding == "8UC3") return CV_8UC3;
  if (encoding == "rgba8" || encoding == "bgra8") return CV_8UC4;
  if (encoding == "mono8" || encoding == "8UC1") return CV_8UC1;
  return -1;
}

/// @brief What the transcode does with one input channel.
struct ChannelPlan {
  /// @brief Copy payloads through untouched.
  bool copy{false};
  /// @brief Camera name, for the channels being re-encoded.
  std::string camera;
  /// @brief Channel metadata carried over from the input.
  std::map<std::string, std::string> metadata;
  /// @brief Output channel; created on the first message for a re-encoded camera, because
  ///        only then are the frame dimensions known.
  std::optional<foxglove::RawChannel> channel;
  /// @brief Encoder for this camera, built once the first frame gives its dimensions.
  std::unique_ptr<utils::VideoEncoder> encoder;
  /// @brief Frame dimensions the encoder was built for; every later frame must match.
  uint32_t width{0};
  uint32_t height{0};
  /// @brief Frames this camera has encoded.
  size_t frames_encoded{0};
};

/**
 * @brief Build the encoder parameters for one camera stream.
 *
 * @param width Frame width in pixels.
 * @param height Frame height in pixels.
 * @param options Encoder settings for the run.
 * @return Parameters for an H.264 encoder of that size.
 */
utils::VideoEncoder::Params encoder_params_for(int width, int height,
                                               const VideoTranscodeOptions& options) {
  utils::VideoEncoder::Params params;
  params.width = width;
  params.height = height;
  // Nominal rate, for the encoder's stream time base only. True frame timing stays in each
  // message's log time, which is copied from the input unchanged.
  params.fps = trossen::configuration::TROSSEN_MCAP_VIDEO_NOMINAL_FPS;
  params.gop_size = options.keyframe_interval;
  params.encoder = options.encoder;
  params.codec = utils::VideoCodec::H264;
  params.bitrate_kbps = options.bitrate_kbps;
  return params;
}

/**
 * @brief Copy an MCAP schema into the Foxglove writer's schema type.
 *
 * @param source Schema read from the input; the returned Schema points into it, so it must
 *   outlive the Schema.
 * @return The schema in the writer's type.
 */
foxglove::Schema schema_from(const mcap::Schema& source) {
  foxglove::Schema schema;
  schema.name = source.name;
  schema.encoding = source.encoding;
  schema.data = source.data.data();
  schema.data_len = source.data.size();
  return schema;
}

/// @brief Serialize a Foxglove message into a buffer, growing it once if needed.
/// @return Byte length written, or 0 on failure (reason logged to stderr).
template <typename Message>
size_t encode_message(Message& msg, std::vector<uint8_t>& buffer) {
  size_t encoded_len = 0;
  auto result = msg.encode(buffer.data(), buffer.size(), &encoded_len);
  if (result == foxglove::FoxgloveError::BufferTooShort) {
    buffer.resize(encoded_len);
    result = msg.encode(buffer.data(), buffer.size(), &encoded_len);
  }
  if (result != foxglove::FoxgloveError::Ok) {
    std::cerr << "Error: Failed to encode message: " << foxglove::strerror(result) << "\n";
    return 0;
  }
  return encoded_len;
}

/**
 * @brief Translate a compression name to the writer's enum.
 *
 * @param name "zstd", "lz4" or "" for none.
 * @return The writer's compression, or nullopt for an unknown name.
 */
std::optional<foxglove::McapCompression> compression_from(const std::string& name) {
  if (name == "zstd") return foxglove::McapCompression::Zstd;
  if (name == "lz4") return foxglove::McapCompression::Lz4;
  if (name.empty()) return foxglove::McapCompression::None;
  return std::nullopt;
}

/// @brief Convert a decoded frame to what the encoder expects: BGR8.
cv::Mat prepare_frame(const cv::Mat& image, const std::string& encoding) {
  // The encoder converts BGR to YUV itself, so anything else is normalized here,
  // otherwise the red and blue channels swap silently.
  cv::Mat bgr;
  if (encoding == "rgb8") {
    cv::cvtColor(image, bgr, cv::COLOR_RGB2BGR);
  } else if (encoding == "rgba8") {
    cv::cvtColor(image, bgr, cv::COLOR_RGBA2BGR);
  } else if (encoding == "bgra8") {
    cv::cvtColor(image, bgr, cv::COLOR_BGRA2BGR);
  } else if (image.type() == CV_8UC1) {
    cv::cvtColor(image, bgr, cv::COLOR_GRAY2BGR);
  } else {
    // The encoder reads rows `width * 3` bytes apart, so padded rows are packed first.
    bgr = image.isContinuous() ? image : image.clone();
  }
  return bgr;
}

}  // namespace

bool transcode_images_to_video(
  const fs::path& input,
  const fs::path& output,
  const VideoTranscodeOptions& options,
  VideoTranscodeStats& stats) {
  stats = VideoTranscodeStats{};

#ifndef TROSSEN_ENABLE_VIDEO_ENCODE
  (void)input;
  (void)output;
  (void)options;
  std::cerr << "Error: This build has no video encoder. Rebuild with "
               "-DTROSSEN_ENABLE_VIDEO_ENCODE=ON\n";
  return false;
#else
  const auto compression = compression_from(options.compression);
  if (!compression) {
    std::cerr << "Error: Unknown compression '" << options.compression
              << "' (expected \"zstd\", \"lz4\" or \"\" for none)\n";
    return false;
  }

  // The input is opened before anything is created on disk, so a missing or unreadable
  // recording leaves no output folder behind.
  std::ifstream stream(input, std::ios::binary);
  if (!stream.is_open()) {
    std::cerr << "Error: Failed to open " << input << "\n";
    return false;
  }

  mcap::McapReader reader;
  auto status = reader.open(stream);
  if (!status.ok()) {
    std::cerr << "Error: Failed to parse " << input << ": " << status.message << "\n";
    return false;
  }
  // The recorder writes the summary when it closes a file, so a file without one was cut
  // short. Scanning it instead would convert only the part that survived, with no error.
  status = reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan);
  if (!status.ok()) {
    std::cerr << "Error: " << input << " is incomplete (no MCAP summary: " << status.message
              << "); it was probably cut short\n";
    return false;
  }

  mcap::KeyValueMap recording;
  if (!read_recording_metadata(reader, recording)) return false;
  const std::string format_version =
    field_or_empty(recording, trossen_mcap_defs::kFormatVersionKey);
  if (format_version != trossen_mcap_defs::kRecordingFormatVersion) {
    std::cerr << "Error: " << input << " has recording format "
              << (format_version.empty() ? std::string("of no version") : format_version)
              << "; this SDK reads format " << trossen_mcap_defs::kRecordingFormatVersion << "\n";
    return false;
  }
  const std::string dataset_info = field_or_empty(recording, trossen_mcap_defs::kDatasetInfoKey);
  std::map<std::string, DescribedCamera> cameras;
  if (!read_described_cameras(dataset_info, cameras)) return false;
  // The recorder stores every camera the same way, so no raw camera means nothing to encode.
  if (std::none_of(cameras.begin(), cameras.end(),
                   [](const auto& entry) { return entry.second.raw; })) {
    std::cout << input.string() << " is already in compressed format; nothing to do\n";
    stats.already_compressed = true;
    return true;
  }

  // The output directory is created before the path checks, because a path through a
  // directory that does not exist yet (`new/../episode.mcap`) only resolves once it does.
  std::error_code ec;
  if (!output.parent_path().empty()) {
    fs::create_directories(output.parent_path(), ec);
    if (ec) {
      std::cerr << "Error: Failed to create " << output.parent_path() << ": " << ec.message()
                << "\n";
      return false;
    }
  }
  const bool output_exists = fs::exists(output, ec);
  if (ec) {
    std::cerr << "Error: Failed to check " << output << ": " << ec.message() << "\n";
    return false;
  }
  if (output_exists) {
    const bool same_file = fs::equivalent(input, output, ec);
    if (ec) {
      std::cerr << "Error: Failed to compare " << input << " with " << output << ": "
                << ec.message() << "\n";
      return false;
    }
    if (same_file) {
      std::cerr << "Error: Output path is the input file\n";
      return false;
    }
    if (!options.overwrite) {
      std::cerr << "Error: Output already exists: " << output << "\n";
      return false;
    }
  }

  // A `.partial` left by a killed run is replaced. The guard removes it on every failed
  // return and is released once it has been renamed onto the output.
  fs::path partial = output;
  partial += ".partial";
  trossen::utils::RemoveOnExit remove_partial{partial};

  auto context = foxglove::Context::create();
  foxglove::McapWriterOptions writer_options;
  writer_options.context = context;
  const std::string partial_str = partial.string();
  writer_options.path = partial_str;
  writer_options.profile = trossen_mcap_defs::kMcapProfile;
  writer_options.compression = *compression;
  writer_options.chunk_size =
    static_cast<uint64_t>(trossen::configuration::TROSSEN_MCAP_DEFAULT_CHUNK_SIZE_BYTES);
  writer_options.truncate = true;

  auto writer_result = foxglove::McapWriter::create(writer_options);
  if (!writer_result.has_value()) {
    std::cerr << "Error: Failed to open " << partial << " for writing: "
              << foxglove::strerror(writer_result.error()) << "\n";
    return false;
  }
  auto writer = std::move(writer_result.value());

  // Plan every channel before the first message. The writer records a channel with its first
  // message, so a channel the recording never logged to is not in the output.
  std::map<mcap::ChannelId, ChannelPlan> plans;
  // McapReader::channels() and schemas() each return a fresh copy of the map, so the
  // lookups below have to be made against one held copy; an iterator taken straight off a
  // call points into a temporary that is already gone.
  const auto input_channels = reader.channels();
  const auto input_schemas = reader.schemas();

  for (const auto& [channel_id, channel] : input_channels) {
    const auto schema_it = input_schemas.find(channel->schemaId);
    const mcap::Schema* schema =
      schema_it != input_schemas.end() ? schema_it->second.get() : nullptr;

    ChannelPlan plan;
    plan.metadata.insert(channel->metadata.begin(), channel->metadata.end());
    // The recording is raw (checked above), so every camera topic holds RawImage frames.
    const auto camera = cameras.find(channel->topic);
    plan.copy = camera == cameras.end();

    if (plan.copy) {
      auto created = foxglove::RawChannel::create(
        channel->topic, channel->messageEncoding,
        schema ? std::optional<foxglove::Schema>(schema_from(*schema)) : std::nullopt, context,
        plan.metadata.empty() ? std::nullopt
                              : std::optional<std::map<std::string, std::string>>(plan.metadata));
      if (!created.has_value()) {
        std::cerr << "Error: Failed to create channel " << channel->topic << ": "
                  << foxglove::strerror(created.error()) << "\n";
        return false;
      }
      plan.channel.emplace(std::move(created.value()));
    } else {
      plan.camera = camera->second.name;
    }
    plans.emplace(channel_id, std::move(plan));
  }

  // The reader skips a damaged chunk and keeps going, so any read problem fails the
  // transcode rather than leaving the output short of messages.
  bool read_failed = false;
  const auto on_read_problem = [&read_failed](const mcap::Status& problem) {
    on_problem(problem);
    read_failed = true;
  };

  std::vector<uint8_t> buffer(kInitialEncodedBufferSize);

  for (const auto& message_view : reader.readMessages(on_read_problem)) {
    auto plan_it = plans.find(message_view.channel->id);
    // A summary may leave out channels, and their messages would be dropped.
    if (plan_it == plans.end()) {
      std::cerr << "Error: Channel " << message_view.channel->topic
                << " is missing from the MCAP summary\n";
      return false;
    }
    ChannelPlan& plan = plan_it->second;
    const uint64_t log_time = message_view.message.logTime;

    if (plan.copy) {
      auto st = plan.channel->log(message_view.message.data, message_view.message.dataSize,
                                  log_time);
      if (st != foxglove::FoxgloveError::Ok) {
        std::cerr << "Error: Failed to copy message on " << message_view.channel->topic << ": "
                  << foxglove::strerror(st) << "\n";
        return false;
      }
      ++stats.messages_copied;
      continue;
    }

    foxglove::RawImage raw_image;
    if (!raw_image.ParseFromArray(message_view.message.data,
                                  static_cast<int>(message_view.message.dataSize))) {
      std::cerr << "Error: Failed to parse RawImage on " << message_view.channel->topic << "\n";
      return false;
    }

    const int cv_type = cv_type_for_encoding(raw_image.encoding());
    if (cv_type < 0) {
      std::cerr << "Error: Unsupported encoding '" << raw_image.encoding() << "' on "
                << message_view.channel->topic << "\n";
      return false;
    }

    // The pixel view below reads `height` rows `stride` bytes apart, so the buffer is checked
    // to hold them before it is wrapped. A step of 0 means tightly packed rows.
    const size_t row_bytes =
      static_cast<size_t>(raw_image.width()) * static_cast<size_t>(CV_ELEM_SIZE(cv_type));
    const size_t stride = raw_image.step() == 0 ? row_bytes : raw_image.step();
    if (raw_image.width() == 0 || raw_image.height() == 0 || stride < row_bytes ||
        raw_image.data().size() < stride * (raw_image.height() - 1) + row_bytes) {
      std::cerr << "Error: RawImage on " << message_view.channel->topic << " is "
                << raw_image.width() << "x" << raw_image.height() << " with step "
                << raw_image.step() << ", but holds " << raw_image.data().size() << " bytes\n";
      return false;
    }
    if (plan.encoder && (raw_image.width() != plan.width || raw_image.height() != plan.height)) {
      std::cerr << "Error: " << plan.camera << " changed from " << plan.width << "x"
                << plan.height << " to " << raw_image.width() << "x" << raw_image.height()
                << " mid-stream\n";
      return false;
    }

    if (!plan.encoder) {
      plan.width = raw_image.width();
      plan.height = raw_image.height();
      plan.encoder = utils::VideoEncoder::create(
        encoder_params_for(static_cast<int>(raw_image.width()),
                           static_cast<int>(raw_image.height()), options));
      if (!plan.encoder) {
        std::cerr << "Error: Failed to create a video encoder for " << plan.camera << "\n";
        return false;
      }

      auto created = foxglove::RawChannel::create(
        message_view.channel->topic, "protobuf", foxglove::schemas::CompressedVideo::schema(),
        context, plan.metadata);
      if (!created.has_value()) {
        std::cerr << "Error: Failed to create video channel " << message_view.channel->topic
                  << ": " << foxglove::strerror(created.error()) << "\n";
        return false;
      }
      plan.channel.emplace(std::move(created.value()));
      ++stats.cameras_transcoded;

      std::cout << "  " << plan.camera << ": " << raw_image.width() << "x" << raw_image.height()
                << " " << raw_image.encoding() << " -> "
                << utils::video_codec_format(plan.encoder->codec()) << " via "
                << plan.encoder->encoder_name() << "\n";
    }

    const cv::Mat image(static_cast<int>(raw_image.height()), static_cast<int>(raw_image.width()),
                        cv_type, const_cast<char*>(raw_image.data().data()), stride);
    const cv::Mat frame = prepare_frame(image, raw_image.encoding());
    if (frame.empty()) {
      std::cerr << "Error: Could not prepare a frame for " << plan.camera << "\n";
      return false;
    }

    auto packet = plan.encoder->encode(reinterpret_cast<const uint8_t*>(frame.data),
                                       frame.total() * frame.elemSize());

    if (packet.data.empty()) {
      // One packet per frame is an invariant, not a nicety: readers pair frame n of the
      // video with the nth frame-meta message, so a dropped packet shifts every later
      // frame. Fail rather than write a file whose misalignment nothing can detect.
      std::cerr << "Error: Encoder produced no packet for " << plan.camera << " frame "
                << plan.frames_encoded << "\n";
      return false;
    }

    foxglove::schemas::CompressedVideo video_message;
    video_message.timestamp =
      foxglove::schemas::Timestamp{.sec = static_cast<uint32_t>(raw_image.timestamp().seconds()),
                                   .nsec = static_cast<uint32_t>(raw_image.timestamp().nanos())};
    video_message.frame_id = raw_image.frame_id();
    video_message.format = utils::video_codec_format(plan.encoder->codec());
    video_message.data.assign(packet.data.begin(), packet.data.end());

    const size_t payload_len = encode_message(video_message, buffer);
    if (payload_len == 0) return false;

    auto st = plan.channel->log(reinterpret_cast<const std::byte*>(buffer.data()), payload_len,
                                log_time);
    if (st != foxglove::FoxgloveError::Ok) {
      std::cerr << "Error: Failed to write a video frame for " << plan.camera << ": "
                << foxglove::strerror(st) << "\n";
      return false;
    }
    ++plan.frames_encoded;
    ++stats.color_frames_encoded;
  }

  if (read_failed) {
    std::cerr << "Error: Failed to read every message from " << input << "\n";
    return false;
  }

  // Channels hold a reference to the writer's context, so they have to go first.
  plans.clear();
  auto close_status = writer.close();
  if (close_status != foxglove::FoxgloveError::Ok) {
    std::cerr << "Error: Failed to close " << partial << ": " << foxglove::strerror(close_status)
              << "\n";
    return false;
  }
  fs::rename(partial, output, ec);
  if (ec) {
    std::cerr << "Error: Failed to move " << partial << " to " << output << ": " << ec.message()
              << "\n";
    return false;
  }
  remove_partial.release();
  return true;
#endif
}

}  // namespace trossen::io::backends
