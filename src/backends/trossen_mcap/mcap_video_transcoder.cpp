/**
 * @file mcap_video_transcoder.cpp
 * @brief Implementation of the raw-image to compressed-video MCAP transcode.
 *
 * Reads with the MCAP library (the reader implementation lives in mcap_dataset_loader.cpp,
 * so this unit must not define MCAP_IMPLEMENTATION) and writes with the Foxglove SDK
 * writer, the same one the recorder uses, so an output file is indistinguishable from a
 * recording made with video storage enabled.
 */

#include "trossen_sdk/io/backends/trossen_mcap/mcap_video_transcoder.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "foxglove/channel.hpp"
#include "foxglove/error.hpp"
#include "foxglove/mcap.hpp"
#include "foxglove/schemas.hpp"
#include "mcap/reader.hpp"

#include "RawImage.pb.h"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"
#include "trossen_sdk/utils/video_encoder.hpp"

namespace fs = std::filesystem;

namespace trossen::io::backends {

namespace {

/// @brief Size the encode buffer starts at; grown once if a message needs more.
constexpr size_t kInitialEncodedBufferSize = 4 * 1024 * 1024;

/// @brief Schema name of the uncompressed camera messages this tool converts.
constexpr char kRawImageSchema[] = "foxglove.RawImage";

/// @brief Schema name of the compressed camera messages it produces.
constexpr char kCompressedVideoSchema[] = "foxglove.CompressedVideo";

/// @brief mcap reader callback: log a recoverable parsing issue and keep reading.
void on_problem(const mcap::Status& problem) {
  std::cerr << "Warning: MCAP parsing issue: " << problem.message << "\n";
}

/// @brief True when a topic is a camera image stream (`/cameras/<name>/image`).
bool is_camera_image_topic(const std::string& topic) {
  const std::string prefix = trossen_mcap_defs::kCameraTopicPrefix;
  const std::string suffix = trossen_mcap_defs::kImageTopicSuffix;
  return topic.size() > prefix.size() + suffix.size() &&
         topic.compare(0, prefix.size(), prefix) == 0 &&
         topic.compare(topic.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// @brief Recover the camera name a camera topic was built from.
std::string camera_name_of(const std::string& topic) {
  const size_t prefix_len = std::string(trossen_mcap_defs::kCameraTopicPrefix).size();
  const size_t suffix_len = std::string(trossen_mcap_defs::kImageTopicSuffix).size();
  return topic.substr(prefix_len, topic.size() - prefix_len - suffix_len);
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
  /// @brief Channel metadata carried over from the input, plus `video_format`.
  std::map<std::string, std::string> metadata;
  /// @brief Output channel; created on the first message for a re-encoded camera, because
  ///        only then are the frame dimensions known.
  std::optional<foxglove::RawChannel> channel;
  /// @brief Encoder for this camera, built once the first frame gives its dimensions.
  std::unique_ptr<utils::VideoEncoder> encoder;
};

/// @brief Build the encoder parameters for one camera stream.
utils::VideoEncoder::Params encoder_params_for(int width, int height,
                                               const std::string& encoder_name,
                                               const VideoTranscodeOptions& options) {
  utils::VideoEncoder::Params params;
  params.width = width;
  params.height = height;
  // Nominal rate, for the encoder's stream time base only. True frame timing stays in each
  // message's log time, which is copied from the input unchanged.
  params.fps = trossen::configuration::TROSSEN_MCAP_VIDEO_NOMINAL_FPS;
  params.gop_size = options.keyframe_interval;
  params.encoder = encoder_name;
  params.codec = utils::VideoCodec::H264;
  params.bitrate_kbps = options.bitrate_kbps;
  return params;
}

/// @brief Copy an MCAP schema into the Foxglove writer's schema type.
///
/// The returned Schema points into `source`, which must outlive it.
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

/// @brief Translate a compression name to the writer's enum, defaulting to none.
foxglove::McapCompression compression_from(const std::string& name) {
  if (name == "zstd") return foxglove::McapCompression::Zstd;
  if (name == "lz4") return foxglove::McapCompression::Lz4;
  if (!name.empty()) {
    std::cerr << "Warning: Unknown compression '" << name << "' (using none)\n";
  }
  return foxglove::McapCompression::None;
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
    bgr = image;
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
  std::error_code ec;
  if (fs::exists(output) && fs::equivalent(input, output, ec)) {
    std::cerr << "Error: Output path is the input file\n";
    return false;
  }
  if (fs::exists(output) && !options.overwrite) {
    std::cerr << "Error: Output already exists: " << output << " (pass --overwrite)\n";
    return false;
  }
  if (!output.parent_path().empty()) {
    fs::create_directories(output.parent_path(), ec);
  }

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
  status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
  if (!status.ok()) {
    std::cerr << "Error: Failed to read MCAP summary: " << status.message << "\n";
    return false;
  }

  auto context = foxglove::Context::create();
  foxglove::McapWriterOptions writer_options;
  writer_options.context = context;
  const std::string output_str = output.string();
  writer_options.path = output_str;
  writer_options.profile = "trossen";
  writer_options.compression = compression_from(options.compression);
  writer_options.chunk_size =
    static_cast<uint64_t>(trossen::configuration::TROSSEN_MCAP_DEFAULT_CHUNK_SIZE_BYTES);
  writer_options.truncate = options.overwrite;

  auto writer_result = foxglove::McapWriter::create(writer_options);
  if (!writer_result.has_value()) {
    std::cerr << "Error: Failed to open " << output << " for writing: "
              << foxglove::strerror(writer_result.error()) << "\n";
    return false;
  }
  auto writer = std::move(writer_result.value());

  // Plan every channel before the first message, so a channel that only needs copying is
  // advertised in the output even if the recording never logged to it.
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
    const std::string schema_name = schema ? schema->name : "";

    ChannelPlan plan;
    plan.metadata.insert(channel->metadata.begin(), channel->metadata.end());
    plan.copy = !(is_camera_image_topic(channel->topic) && schema_name == kRawImageSchema);

    if (plan.copy) {
      if (is_camera_image_topic(channel->topic) && schema_name == kCompressedVideoSchema) {
        ++stats.cameras_passthrough;
      }
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
      plan.camera = camera_name_of(channel->topic);
      ++stats.cameras_transcoded;
    }
    plans.emplace(channel_id, std::move(plan));
  }

  std::vector<uint8_t> buffer(kInitialEncodedBufferSize);

  for (const auto& message_view : reader.readMessages(on_problem)) {
    auto plan_it = plans.find(message_view.channel->id);
    if (plan_it == plans.end()) continue;
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

    if (!plan.encoder) {
      plan.encoder = utils::VideoEncoder::create(
        encoder_params_for(static_cast<int>(raw_image.width()),
                           static_cast<int>(raw_image.height()), options.encoder, options));
      if (!plan.encoder) {
        std::cerr << "Error: Failed to create a video encoder for " << plan.camera << "\n";
        return false;
      }

      // Recorded in the channel metadata so a reader can tell which codec a stream carries
      // without decoding a packet to find out.
      plan.metadata["stream_type"] = "color";
      plan.metadata["video_format"] = utils::video_codec_format(plan.encoder->codec());

      auto created = foxglove::RawChannel::create(
        message_view.channel->topic, "protobuf", foxglove::schemas::CompressedVideo::schema(),
        context, plan.metadata);
      if (!created.has_value()) {
        std::cerr << "Error: Failed to create video channel " << message_view.channel->topic
                  << ": " << foxglove::strerror(created.error()) << "\n";
        return false;
      }
      plan.channel.emplace(std::move(created.value()));

      std::cout << "  " << plan.camera << ": " << raw_image.width() << "x" << raw_image.height()
                << " " << raw_image.encoding() << " -> "
                << utils::video_codec_format(plan.encoder->codec()) << " via "
                << plan.encoder->encoder_name() << "\n";
    }

    const cv::Mat image(static_cast<int>(raw_image.height()), static_cast<int>(raw_image.width()),
                        cv_type, const_cast<char*>(raw_image.data().data()), raw_image.step());
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
                << stats.color_frames_encoded << "\n";
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
    ++stats.color_frames_encoded;
  }

  // Channels hold a reference to the writer's context, so they have to go first.
  plans.clear();
  auto close_status = writer.close();
  if (close_status != foxglove::FoxgloveError::Ok) {
    std::cerr << "Error: Failed to close " << output << ": " << foxglove::strerror(close_status)
              << "\n";
    return false;
  }
  reader.close();
  return true;
#endif
}

}  // namespace trossen::io::backends
