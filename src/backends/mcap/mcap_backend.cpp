/**
 * @file mcap_backend.cpp
 * @brief Implementation of McapBackend for Trossen SDK.
 */

#include <chrono>
#include <iostream>
#include <unordered_set>

#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "opencv2/imgcodecs.hpp"

#include "JointState.pb.h"
#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/io/backend_registry.hpp"
#include "trossen_sdk/io/backends/mcap/mcap_backend.hpp"
#include "trossen_sdk/version.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/types/backends/mcap_backend_config.hpp"



namespace trossen::io::backends {

REGISTER_BACKEND(McapBackend, "mcap")

McapBackend::McapBackend(
  Config cfg,
  const ProducerMetadataList&)
  : io::Backend(), cfg_(std::move(cfg)) {


  // This allows us to access the global configuration for the Mcap backend
  // without passing it explicitly.

  test_config_ = GlobalConfig::instance().get_as<McapBackendConfig>("mcap_backend");
  if (!test_config_) {
        std::cerr << "Backend config not found!" << std::endl;
        return;
  }
  // Print the stored values
  std::cout << "================= MCAP Backend Config =================" << std::endl;
  std::cout << "Output Dir: " << test_config_->output_dir << std::endl;
  std::cout << "Robot Name: " << test_config_->robot_name << std::endl;
  std::cout << "Chunk Size Bytes: " << test_config_->chunk_size_bytes << std::endl;
  std::cout << "Compression: " << test_config_->compression << std::endl;
  std::cout << "Dataset ID: " << test_config_->dataset_id << std::endl;
  std::cout << "Episode Index: " << test_config_->episode_index << std::endl;
  std::cout << "======================================================" << std::endl;

  }
McapBackend::~McapBackend() { close(); }

void McapBackend::preprocess_episode()
{
  // No-op Delete If not needed
}

bool McapBackend::open() {
  std::scoped_lock lk(writer_mutex_);

  // Early return if already opened
  if (opened_) {
    return true;
  }
  std::ostringstream oss;
  oss << cfg_.output_path << "/"
      << cfg_.dataset_id << "/"
      << "episode_" << std::setw(6) << std::setfill('0') << episode_index_ << ".mcap";
  // Parse configs
  path_ = std::filesystem::path(oss.str());

  // Create Foxglove context
  context_ = foxglove::Context::create();

  // Store path as string
  std::string path_str = path_.string();

  // Configure MCAP writer options
  foxglove::McapWriterOptions opts;
  opts.context = context_;
  opts.path = path_str;
  opts.profile = "trossen";
  opts.chunk_size = cfg_.chunk_size_bytes;

  if (cfg_.compression == "zstd") {
    opts.compression = foxglove::McapCompression::Zstd;
  } else if (cfg_.compression == "lz4") {
    opts.compression = foxglove::McapCompression::Lz4;
  } else if (cfg_.compression.empty()) {
    opts.compression = foxglove::McapCompression::None;
  } else {
    std::cerr << "Unknown compression option: " << cfg_.compression << " (falling back to none)\n";
    opts.compression = foxglove::McapCompression::None;
  }
  // Check if the output path parent directory exists
  auto parent_path = path_.parent_path();
  if (!parent_path.empty() && !std::filesystem::exists(parent_path)) {
    try {
      std::filesystem::create_directories(parent_path);
    } catch (const std::exception& e) {
      std::cerr << "Failed to create parent directories for MCAP file at "
                << parent_path << ": " << e.what() << "\n";
      return false;
    }
  }

  // Open mcap writer
  auto writer_result = foxglove::McapWriter::create(opts);
  if (!writer_result.has_value()) {
    std::cerr << "Failed to open MCAP file: " << foxglove::strerror(writer_result.error()) << "\n";
    return false;
  }
  writer_ = std::move(writer_result.value());
  opened_ = true;

  register_schemas_once();

  // Write MCAP file-level metadata
  std::map<std::string, std::string> metadata;
  metadata["tool_version"] = trossen::core::version();
  metadata["dataset_id"] = cfg_.dataset_id;
  metadata["episode_index"] = std::to_string(episode_index_);
  auto now = trossen::data::now_real();
  metadata["recording_start_time"] = std::to_string(now.to_ns());

  auto st = writer_->writeMetadata("trossen_sdk_recording", metadata.begin(), metadata.end());
  if (st != foxglove::FoxgloveError::Ok) {
    std::cerr << "Failed to write metadata: " << foxglove::strerror(st) << "\n";
  }

  return true;
}

void McapBackend::close() {
  std::scoped_lock lk(writer_mutex_);
  if (!opened_) {
    return;
  }
  // Close channels
  if (joint_channel_) {
    joint_channel_->close();
  }
  for (auto& [name, channel] : image_channels_) {
    channel.close();
  }
  if (writer_) {
    auto st = writer_->close();
    if (st != foxglove::FoxgloveError::Ok) {
      std::cerr << "Failed to close MCAP writer: " << foxglove::strerror(st) << "\n";
    }
  }
  opened_ = false;
}

void McapBackend::flush() {
  std::scoped_lock lk(writer_mutex_);
  if (!opened_) {
    return;
  }
}

void McapBackend::write(const data::RecordBase& record) {
  std::scoped_lock lk(writer_mutex_);
  if (!opened_) return;

  // TODO(lukeschmitt-tr): should we use dynamic_cast for this?
  if (auto img = dynamic_cast<const data::ImageRecord*>(&record)) {
    write_image_record(*img);
    return;
  }

  if (auto js = dynamic_cast<const data::JointStateRecord*>(&record)) {
    write_jointstate_record(*js);
    return;
  }
}

void McapBackend::write_batch(std::span<const data::RecordBase* const> records) {
  // Acquire once for the batch to avoid per-message lock/unlock churn
  std::scoped_lock lk(writer_mutex_);
  if (!opened_) return;
  for (auto* r : records) if (r) {
    if (auto img = dynamic_cast<const data::ImageRecord*>(r)) {
      write_image_record(*img);
      continue;
    }
    if (auto js = dynamic_cast<const data::JointStateRecord*>(r)) {
      write_jointstate_record(*js);
      continue;
    }
  }
}

void McapBackend::ensure_jointstate_channel() {
  // Check if the channel already exists
  if (joint_channel_.has_value()) {
    return;
  }

  // Create schema
  foxglove::Schema schema;
  schema.name = "trossen_sdk.msg.JointState";
  schema.encoding = "protobuf";
  schema.data = reinterpret_cast<const std::byte*>(schema_data_.data());
  schema.data_len = schema_data_.size();

  // Create channel
  auto channel_result = foxglove::RawChannel::create(
    mcapdefs::joint_state_topic(cfg_.robot_name),
    "protobuf",
    schema,
    context_,
    std::nullopt);

  if (!channel_result.has_value()) {
    std::cerr << "Failed to create joint state channel: "
              << foxglove::strerror(channel_result.error()) << "\n";
    return;
  }

  joint_channel_ = std::move(channel_result.value());
}

void McapBackend::ensure_image_channel(const std::string& camera_name) {
  // Color path: delegate to generalized ensure with minimal metadata
  ensure_image_channel_with_metadata(camera_name, { {"stream_type", "color"} });
}

void McapBackend::ensure_image_channel_with_metadata(
  const std::string& camera_name,
  const std::unordered_map<std::string, std::string>& metadata) {
  auto it = image_channels_.find(camera_name);
  if (it != image_channels_.end()) {
    return;  // already exists
  }

  // Use Foxglove SDK's built-in RawImage schema
  foxglove::Schema schema = foxglove::schemas::RawImage::schema();

  // Convert metadata to std::map
  std::map<std::string, std::string> channel_metadata(metadata.begin(), metadata.end());

  // Create channel
  auto channel_result = foxglove::RawChannel::create(
    mcapdefs::image_topic(camera_name),
    "protobuf",
    schema,
    context_,
    channel_metadata);

  if (!channel_result.has_value()) {
    std::cerr << "Failed to create image channel: "
              << foxglove::strerror(channel_result.error()) << "\n";
    return;
  }

  image_channels_.emplace(camera_name, std::move(channel_result.value()));
}

void McapBackend::write_jointstate_record(const data::JointStateRecord& js) {
  ensure_jointstate_channel();
  trossen_sdk::msg::JointState out;
  auto* ts = out.mutable_ts();

  // Set monotonic timestamp
  auto* mono = ts->mutable_monotonic();
  mono->set_seconds(js.ts.monotonic.sec);
  mono->set_nanos(js.ts.monotonic.nsec);

  // Set realtime timestamp
  auto* real = ts->mutable_realtime();
  real->set_seconds(js.ts.realtime.sec);
  real->set_nanos(js.ts.realtime.nsec);

  out.set_seq(js.seq);
  out.mutable_positions()->Reserve(js.positions.size());
  for (auto v : js.positions) out.add_positions(v);
  out.mutable_velocities()->Reserve(js.velocities.size());
  for (auto v : js.velocities) out.add_velocities(v);
  out.mutable_efforts()->Reserve(js.efforts.size());
  for (auto v : js.efforts) out.add_efforts(v);
  std::string payload;
  out.SerializeToString(&payload);

  auto st = joint_channel_->log(
    reinterpret_cast<const std::byte*>(payload.data()),
    payload.size(),
    js.ts.realtime.to_ns());

  if (st != foxglove::FoxgloveError::Ok) {
    std::cerr << "Failed to write joint state: " << foxglove::strerror(st) << "\n";
  } else {
    ++stats_.joint_states_written;
  }
}

void McapBackend::write_image_record(const data::ImageRecord& img) {
  // Determine if this is a depth frame based on encoding or topic
  const bool depth =
    is_depth_encoding(img.encoding) || is_depth_topic(mcapdefs::image_topic(img.id));
  if (depth) {
    // Depth metadata; attempt to parse scale if provided in encoding (future) - for now leave
    // blank
    //
    // We cannot know depth_scale_m here without augmenting ImageRecord; future extension could
    // pass via id pattern.
    //
    // Minimal metadata: stream_type + semantics if derivable from encoding.
    std::unordered_map<std::string, std::string> md;
    md["stream_type"] = "depth";
    if (img.encoding == "depth16") {
      md["depth_encoding_semantics"] = "uint16_scaled";
      // depth_scale_m left to producer-specific channel creation path later when available
    } else if (img.encoding == "32FC1") {
      md["depth_encoding_semantics"] = "float_m";
    }
    ensure_image_channel_with_metadata(img.id, md);
  } else {
    ensure_image_channel(img.id);
  }
  foxglove::schemas::RawImage imsg;
  imsg.timestamp = foxglove::schemas::Timestamp{
    .sec = static_cast<uint32_t>(img.ts.realtime.sec),
    .nsec = static_cast<uint32_t>(img.ts.realtime.nsec)
  };
  imsg.frame_id = img.id;
  imsg.width = img.width;
  imsg.height = img.height;
  imsg.encoding = img.encoding;
  imsg.step = static_cast<uint32_t>(img.image.step);
  // Copy image data to std::vector<std::byte>
  const std::byte* data_ptr = reinterpret_cast<const std::byte*>(img.image.data);
  size_t data_size = img.image.total() * img.image.elemSize();
  imsg.data.assign(data_ptr, data_ptr + data_size);

  // Encode to buffer
  std::vector<uint8_t> payload(MCAP_INITIAL_ENCODED_BUFFER_SIZE);
  size_t encoded_len = 0;
  auto encode_result = imsg.encode(payload.data(), payload.size(), &encoded_len);

  if (encode_result == foxglove::FoxgloveError::BufferTooShort) {
    // Resize and try again
    payload.resize(encoded_len);
    encode_result = imsg.encode(payload.data(), payload.size(), &encoded_len);
  }

  if (encode_result != foxglove::FoxgloveError::Ok) {
    std::cerr << "Failed to encode image: " << foxglove::strerror(encode_result) << "\n";
    return;
  }

  auto st = image_channels_.at(img.id).log(
    reinterpret_cast<const std::byte*>(payload.data()),
    encoded_len,
    img.ts.realtime.to_ns());

  if (st != foxglove::FoxgloveError::Ok) {
    std::cerr << "Failed to write image: " << foxglove::strerror(st) << "\n";
  } else {
    if (depth) {
      ++stats_.depth_images_written;
    } else {
      ++stats_.images_written;
    }
  }
}

void McapBackend::register_schemas_once() {
  // Build descriptor set from generated pool, including transitive dependencies
  const auto* pool = google::protobuf::DescriptorPool::generated_pool();
  google::protobuf::FileDescriptorSet set;

  const char* roots[] = {
    "trossen_sdk/io/backends/mcap/proto/JointState.proto",
  };

  std::unordered_set<std::string> visited;
  std::function<void(const google::protobuf::FileDescriptor*)> add_with_deps;
  add_with_deps = [&](const google::protobuf::FileDescriptor* fd) {
    if (!fd) {
      return;
    }
    if (!visited.insert(fd->name()).second){
      // already added
       return;
    }
    // Recurse first so dependencies appear earlier (order not strictly required)
    for (int i = 0; i < fd->dependency_count(); ++i) {
      add_with_deps(fd->dependency(i));
    }
    fd->CopyTo(set.add_file());
  };

  for (auto* fname : roots) {
    const google::protobuf::FileDescriptor* fd = pool->FindFileByName(fname);
    if (!fd) {
      // Try basename fallback if the compiler emitted only the base name
      std::string base(fname);
      if (auto pos = base.find_last_of('/'); pos != std::string::npos) {
        base = base.substr(pos + 1);
      }
      fd = pool->FindFileByName(base);
    }
    add_with_deps(fd);
  }

  std::string blob;
  set.SerializeToString(&blob);
  schema_data_ = blob;
}

bool McapBackend::is_depth_topic(const std::string& topic) {
  // Simple heuristic: contains "/depth/" before final name
  return topic.find("/depth/") != std::string::npos;
}

bool McapBackend::is_depth_encoding(const std::string& enc) {
  return enc == "depth16" || enc == "32FC1" || enc == "16UC1";  // allow alias
}


uint32_t McapBackend::scan_existing_episodes() {
  std::filesystem::path base_path = std::filesystem::path(cfg_.output_path) / cfg_.dataset_id;
  std::cout << "Scanning existing episodes in: " << base_path << std::endl;
  // If directory doesn't exist, return 0
  if (!std::filesystem::exists(base_path)) {
    return 0;
  }

  // If not a directory, return 0
  if (!std::filesystem::is_directory(base_path)) {
    std::cerr << "Warning: base_path exists but is not a directory: " << base_path << std::endl;
    return 0;
  }

  // Pattern: episode_NNNNNN.mcap (6-digit zero-padded) Regex to match episode files
  std::regex episode_pattern(R"(episode_(\d{6})\.mcap)");

  uint32_t max_index = 0;
  bool found_any = false;

  try {
    // Iterate through directory entries
    for (const auto& entry : std::filesystem::directory_iterator(base_path)) {
      // Skip if not a regular file
      if (!entry.is_regular_file()) {
        continue;
      }

      // Get filename only (not full path)
      std::string filename = entry.path().filename().string();

      // Try to match against episode pattern
      std::smatch match;
      if (std::regex_match(filename, match, episode_pattern)) {
        // Extract the numeric index from capture group 1
        std::string index_str = match[1].str();
        uint32_t index = static_cast<uint32_t>(std::stoul(index_str));

        // Track maximum index found
        if (!found_any || index > max_index) {
          max_index = index;
          found_any = true;
        }
      }
      // Silently ignore non-episode files (as per design doc)
    }
  } catch (const std::filesystem::filesystem_error& e) {
    std::cerr << "Filesystem error while scanning episodes: " << e.what() << std::endl;
    return 0;
  }

  // Return max_index + 1, or 0 if no episodes found
  return found_any ? (max_index + 1) : 0;
}

}  // namespace trossen::io::backends
