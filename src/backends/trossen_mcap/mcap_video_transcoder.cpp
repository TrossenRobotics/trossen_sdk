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
#include <optional>
#include <string>
#include <vector>

#include "foxglove/channel.hpp"
#include "foxglove/error.hpp"
#include "foxglove/mcap.hpp"
#include "mcap/reader.hpp"

#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"

namespace fs = std::filesystem;

namespace trossen::io::backends {

namespace {

/// @brief Schema name of compressed camera messages.
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

/// @brief What the transcode does with one input channel.
struct ChannelPlan {
  /// @brief Channel metadata carried over from the input, plus `video_format`.
  std::map<std::string, std::string> metadata;
  /// @brief Output channel.
  std::optional<foxglove::RawChannel> channel;
};

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

/// @brief Translate a compression name to the writer's enum, defaulting to none.
foxglove::McapCompression compression_from(const std::string& name) {
  if (name == "zstd") return foxglove::McapCompression::Zstd;
  if (name == "lz4") return foxglove::McapCompression::Lz4;
  if (!name.empty()) {
    std::cerr << "Warning: Unknown compression '" << name << "' (using none)\n";
  }
  return foxglove::McapCompression::None;
}

}  // namespace

bool transcode_images_to_video(
  const fs::path& input,
  const fs::path& output,
  const VideoTranscodeOptions& options,
  VideoTranscodeStats& stats) {
  stats = VideoTranscodeStats{};

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
    plans.emplace(channel_id, std::move(plan));
  }

  for (const auto& message_view : reader.readMessages(on_problem)) {
    auto plan_it = plans.find(message_view.channel->id);
    if (plan_it == plans.end()) continue;
    ChannelPlan& plan = plan_it->second;

    auto st = plan.channel->log(message_view.message.data, message_view.message.dataSize,
                                message_view.message.logTime);
    if (st != foxglove::FoxgloveError::Ok) {
      std::cerr << "Error: Failed to copy message on " << message_view.channel->topic << ": "
                << foxglove::strerror(st) << "\n";
      return false;
    }
    ++stats.messages_copied;
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
}

}  // namespace trossen::io::backends
