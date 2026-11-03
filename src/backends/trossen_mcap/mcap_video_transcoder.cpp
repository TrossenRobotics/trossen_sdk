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
#include <utility>
#include <vector>

#include "foxglove/channel.hpp"
#include "foxglove/error.hpp"
#include "foxglove/mcap.hpp"
#include "mcap/reader.hpp"

#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"

namespace fs = std::filesystem;

namespace trossen::io::backends {

namespace {

/// @brief mcap reader callback: log a recoverable parsing issue and keep reading.
void on_problem(const mcap::Status& problem) {
  std::cerr << "Warning: MCAP parsing issue: " << problem.message << "\n";
}

/// @brief Read `image_encoding` from the recording's metadata record.
///
/// @param encoding Set to the value, or left empty when the record or key is absent.
/// @return false when the record exists but cannot be read (reason logged to stderr).
bool read_image_encoding(mcap::McapReader& reader, std::string& encoding) {
  encoding.clear();
  const auto& indexes = reader.metadataIndexes();
  const auto index = indexes.find(trossen_mcap_defs::kRecordingMetadataName);
  if (index == indexes.end()) return true;
  auto* data_source = reader.dataSource();
  if (!data_source) {
    std::cerr << "Error: No data source to read the recording metadata from\n";
    return false;
  }
  mcap::Record raw_record;
  auto status = mcap::McapReader::ReadRecord(*data_source, index->second.offset, &raw_record);
  mcap::Metadata record;
  if (status.ok()) status = mcap::McapReader::ParseMetadata(raw_record, &record);
  if (!status.ok()) {
    std::cerr << "Error: Failed to read the recording metadata: " << status.message << "\n";
    return false;
  }
  const auto value = record.metadata.find(trossen_mcap_defs::kImageEncodingKey);
  if (value != record.metadata.end()) encoding = value->second;
  return true;
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

/// @brief Translate a compression name to the writer's enum; nullopt for an unknown name.
std::optional<foxglove::McapCompression> compression_from(const std::string& name) {
  if (name == "zstd") return foxglove::McapCompression::Zstd;
  if (name == "lz4") return foxglove::McapCompression::Lz4;
  if (name.empty()) return foxglove::McapCompression::None;
  return std::nullopt;
}

/**
 * @brief RAII guard that deletes a file on destruction unless released.
 *
 * Holds the in-progress output, so every failed return removes it and leaves any earlier
 * output at the final path untouched.
 */
class RemoveOnExit {
public:
  explicit RemoveOnExit(fs::path path) : path_(std::move(path)) {}
  ~RemoveOnExit() {
    if (path_.empty()) return;
    std::error_code ec;
    fs::remove(path_, ec);
  }

  RemoveOnExit(const RemoveOnExit&) = delete;
  RemoveOnExit& operator=(const RemoveOnExit&) = delete;
  RemoveOnExit(RemoveOnExit&&) = delete;
  RemoveOnExit& operator=(RemoveOnExit&&) = delete;

  /// @brief Keep the file: it has been moved to its final path.
  void release() { path_.clear(); }

private:
  fs::path path_;
};

}  // namespace

bool transcode_images_to_video(
  const fs::path& input,
  const fs::path& output,
  const VideoTranscodeOptions& options,
  VideoTranscodeStats& stats) {
  stats = VideoTranscodeStats{};

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

  // A recording without the key predates video storage, so its frames are raw.
  std::string image_encoding;
  if (!read_image_encoding(reader, image_encoding)) return false;
  if (image_encoding == trossen::configuration::TROSSEN_MCAP_IMAGE_ENCODING_VIDEO) {
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
  }
  if (output_exists && !options.overwrite) {
    std::cerr << "Error: Output already exists: " << output << " (pass --overwrite)\n";
    return false;
  }

  // Written beside the output and renamed onto it only once complete, so a failed run never
  // leaves a partial file at the output path. A `.partial` left by a killed run is replaced.
  fs::path partial = output;
  partial += ".partial";
  RemoveOnExit remove_partial(partial);

  auto context = foxglove::Context::create();
  foxglove::McapWriterOptions writer_options;
  writer_options.context = context;
  const std::string partial_str = partial.string();
  writer_options.path = partial_str;
  writer_options.profile = "trossen";
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

  // The reader skips a damaged chunk and keeps going, so any read problem fails the
  // transcode rather than leaving the output short of messages.
  bool read_failed = false;
  const auto on_read_problem = [&read_failed](const mcap::Status& problem) {
    on_problem(problem);
    read_failed = true;
  };

  for (const auto& message_view : reader.readMessages(on_read_problem)) {
    auto plan_it = plans.find(message_view.channel->id);
    // A summary may leave out channels, and their messages would be dropped.
    if (plan_it == plans.end()) {
      std::cerr << "Error: Channel " << message_view.channel->topic
                << " is missing from the MCAP summary\n";
      return false;
    }
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

  if (read_failed) {
    std::cerr << "Error: Failed to read every message from " << input << "\n";
    return false;
  }

  // Channels hold a reference to the writer's context, so they have to go first.
  plans.clear();
  auto close_status = writer.close();
  if (close_status != foxglove::FoxgloveError::Ok) {
    std::cerr << "Error: Failed to close " << output << ": " << foxglove::strerror(close_status)
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
  reader.close();
  return true;
}

}  // namespace trossen::io::backends
