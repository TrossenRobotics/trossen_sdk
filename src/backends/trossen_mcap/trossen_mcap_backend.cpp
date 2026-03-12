/**
 * @file trossen_mcap_backend.cpp
 * @brief Implementation of TrossenMCAPBackend for Trossen SDK.
 */

#include <chrono>
#include <iostream>
#include <unordered_set>

#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "opencv2/imgcodecs.hpp"

#include "JointState.pb.h"
#include "Odometry2D.pb.h"
#include "nlohmann/json.hpp"
#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/io/backend_registry.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_backend.hpp"
#include "trossen_sdk/version.hpp"

namespace trossen::io::backends {

REGISTER_BACKEND(TrossenMCAPBackend, "trossen_mcap")

TrossenMCAPBackend::TrossenMCAPBackend(
  const ProducerMetadataList& producer_metadata)
  : io::Backend(), producer_metadata_(producer_metadata) {
  // This allows us to access the global configuration for the TrossenMCAP backend
  // without passing it explicitly.
  cfg_ = trossen::configuration::GlobalConfig::instance()
           .get_as<trossen::configuration::TrossenMCAPBackendConfig>(
             "trossen_mcap_backend");
  if (!cfg_) {
        std::cerr << "Backend config not found!" << std::endl;
        return;
  }
  // If the root path is empty, set to default
  if (cfg_->root.empty()) {
      cfg_->root = trossen::io::backends::get_default_root_path().string();
  }
  // Print the stored values
  std::cout << "================= TrossenMCAP Backend Config =================" << std::endl;
  std::cout << "Root Dir: " << cfg_->root << std::endl;
  std::cout << "Robot Name: " << cfg_->robot_name << std::endl;
  std::cout << "Chunk Size Bytes: " << cfg_->chunk_size_bytes << std::endl;
  std::cout << "Compression: " << cfg_->compression << std::endl;
  std::cout << "Dataset ID: " << cfg_->dataset_id << std::endl;
  std::cout << "Episode Index: " << cfg_->episode_index << std::endl;
  std::cout << "======================================================" << std::endl;
  }
TrossenMCAPBackend::~TrossenMCAPBackend() { close(); }

void TrossenMCAPBackend::preprocess_episode()
{
  // No-op Delete If not needed
}

bool TrossenMCAPBackend::open() {
  std::scoped_lock lk(writer_mutex_);

  // Early return if already opened
  if (opened_) {
    return true;
  }
  std::ostringstream oss;
  oss << cfg_->root << "/"
      << cfg_->dataset_id << "/"
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
  opts.chunk_size = cfg_->chunk_size_bytes;

  if (cfg_->compression == "zstd") {
    opts.compression = foxglove::McapCompression::Zstd;
  } else if (cfg_->compression == "lz4") {
    opts.compression = foxglove::McapCompression::Lz4;
  } else if (cfg_->compression.empty()) {
    opts.compression = foxglove::McapCompression::None;
  } else {
    std::cerr << "Unknown compression option: " << cfg_->compression << " (falling back to none)\n";
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
  metadata["dataset_id"] = cfg_->dataset_id;
  metadata["robot_name"] = cfg_->robot_name;
  metadata["episode_index"] = std::to_string(episode_index_);
  auto now = trossen::data::now_real();
  metadata["recording_start_time"] = std::to_string(now.to_ns());

  // Build dataset_info JSON from producer metadata (joint names, camera specs, etc.)
  nlohmann::ordered_json dataset_info;
  dataset_info["robot_name"] = cfg_->robot_name;

  for (const auto& producer_meta : producer_metadata_) {
    if (!producer_meta) continue;
    nlohmann::ordered_json stream_info = producer_meta->get_stream_info();
    if (stream_info.empty()) continue;

    // Merge "streams" entries
    if (stream_info.contains("streams")) {
      for (auto& [key, val] : stream_info["streams"].items()) {
        dataset_info["streams"][key] = val;
      }
    }
    // Merge "cameras" entries
    if (stream_info.contains("cameras")) {
      for (auto& [key, val] : stream_info["cameras"].items()) {
        dataset_info["cameras"][key] = val;
      }
    }
    // Mobile base flag
    if (stream_info.value("has_mobile_base", false)) {
      dataset_info["has_mobile_base"] = true;
      if (stream_info.contains("base_velocity_names")) {
        dataset_info["base_velocity_names"] = stream_info["base_velocity_names"];
      }
    }
  }

  metadata["dataset_info"] = dataset_info.dump();

  auto st = writer_->writeMetadata("trossen_sdk_recording", metadata.begin(), metadata.end());
  if (st != foxglove::FoxgloveError::Ok) {
    std::cerr << "Failed to write metadata: " << foxglove::strerror(st) << "\n";
  }

  return true;
}

void TrossenMCAPBackend::close() {
  std::scoped_lock lk(writer_mutex_);
  if (!opened_) {
    return;
  }
  // Close channels
  for (auto& [stream_id, channel] : joint_channels_) {
    channel.close();
  }
  for (auto& [stream_id, channel] : odometry_2d_channels_) {
    channel.close();
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
  joint_channels_.clear();
  image_channels_.clear();
  odometry_2d_channels_.clear();
  opened_ = false;
}

void TrossenMCAPBackend::flush() {
  std::scoped_lock lk(writer_mutex_);
  if (!opened_) {
    return;
  }
}

void TrossenMCAPBackend::write(const data::RecordBase& record) {
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

  if (auto mb = dynamic_cast<const data::Odometry2DRecord*>(&record)) {
    write_odometry_2d_record(*mb);
    return;
  }
}

void TrossenMCAPBackend::write_batch(std::span<const data::RecordBase* const> records) {
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
    if (auto mb = dynamic_cast<const data::Odometry2DRecord*>(r)) {
      write_odometry_2d_record(*mb);
      continue;
    }
  }
}

foxglove::RawChannel* TrossenMCAPBackend::ensure_jointstate_channel(const std::string& stream_id) {
  auto it = joint_channels_.find(stream_id);
  if (it != joint_channels_.end()) {
    return &it->second;
  }

  // Create schema
  foxglove::Schema schema;
  schema.name = "trossen_sdk.msg.JointState";
  schema.encoding = "protobuf";
  schema.data = reinterpret_cast<const std::byte*>(schema_data_js_.data());
  schema.data_len = schema_data_js_.size();

  // Create channel with stream-specific topic
  auto channel_result = foxglove::RawChannel::create(
    trossen_mcap_defs::joint_state_topic(stream_id),
    "protobuf",
    schema,
    context_,
    std::nullopt);

  if (!channel_result.has_value()) {
    std::cerr << "Failed to create joint state channel for " << stream_id << ": "
              << foxglove::strerror(channel_result.error()) << "\n";
    return nullptr;
  }

  auto [inserted_it, _] = joint_channels_.emplace(stream_id, std::move(channel_result.value()));
  return &inserted_it->second;
}

foxglove::RawChannel* TrossenMCAPBackend::ensure_image_channel(const std::string& camera_name) {
  return ensure_image_channel_with_metadata(camera_name, { {"stream_type", "color"} });
}

foxglove::RawChannel* TrossenMCAPBackend::ensure_image_channel_with_metadata(
  const std::string& camera_name,
  const std::unordered_map<std::string, std::string>& metadata) {
  auto it = image_channels_.find(camera_name);
  if (it != image_channels_.end()) {
    return &it->second;
  }

  // Use Foxglove SDK's built-in RawImage schema
  foxglove::Schema schema = foxglove::schemas::RawImage::schema();

  // Convert metadata to std::map
  std::map<std::string, std::string> channel_metadata(metadata.begin(), metadata.end());

  // Create channel
  auto channel_result = foxglove::RawChannel::create(
    trossen_mcap_defs::image_topic(camera_name),
    "protobuf",
    schema,
    context_,
    channel_metadata);

  if (!channel_result.has_value()) {
    std::cerr << "Failed to create image channel: "
              << foxglove::strerror(channel_result.error()) << "\n";
    return nullptr;
  }

  auto [inserted_it, _] = image_channels_.emplace(camera_name, std::move(channel_result.value()));
  return &inserted_it->second;
}

void TrossenMCAPBackend::write_jointstate_record(const data::JointStateRecord& js) {
  auto* channel = ensure_jointstate_channel(js.id);
  if (!channel) {
    return;
  }

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

  auto st = channel->log(
    reinterpret_cast<const std::byte*>(payload.data()),
    payload.size(),
    js.ts.realtime.to_ns());

  if (st != foxglove::FoxgloveError::Ok) {
    std::cerr << "Failed to write joint state for " << js.id << ": "
              << foxglove::strerror(st) << "\n";
  } else {
    ++stats_.joint_states_written;
  }
}

foxglove::RawChannel* TrossenMCAPBackend::ensure_odometry_2d_channel(const std::string& stream_id) {
  auto it = odometry_2d_channels_.find(stream_id);
  if (it != odometry_2d_channels_.end()) {
    return &it->second;
  }

  foxglove::Schema schema;
  schema.name = "trossen_sdk.msg.Odometry2D";
  schema.encoding = "protobuf";
  schema.data = reinterpret_cast<const std::byte*>(schema_data_odom2d_.data());
  schema.data_len = schema_data_odom2d_.size();

  auto channel_result = foxglove::RawChannel::create(
    trossen_mcap_defs::odometry_2d_topic(stream_id),
    "protobuf",
    schema,
    context_,
    std::nullopt);

  if (!channel_result.has_value()) {
    std::cerr << "Failed to create odometry 2D channel for " << stream_id << ": "
              << foxglove::strerror(channel_result.error()) << "\n";
    return nullptr;
  }

  auto [inserted_it, _] = odometry_2d_channels_.emplace(
    stream_id, std::move(channel_result.value()));
  return &inserted_it->second;
}

void TrossenMCAPBackend::write_odometry_2d_record(const data::Odometry2DRecord& odom) {
  auto* channel = ensure_odometry_2d_channel(odom.id);
  if (!channel) {
    return;
  }

  trossen_sdk::msg::Odometry2D out;
  auto* ts = out.mutable_ts();

  auto* mono = ts->mutable_monotonic();
  mono->set_seconds(odom.ts.monotonic.sec);
  mono->set_nanos(odom.ts.monotonic.nsec);

  auto* real = ts->mutable_realtime();
  real->set_seconds(odom.ts.realtime.sec);
  real->set_nanos(odom.ts.realtime.nsec);

  out.set_seq(odom.seq);

  auto* pose = out.mutable_pose();
  pose->set_x(odom.pose.x);
  pose->set_y(odom.pose.y);
  pose->set_theta(odom.pose.theta);

  auto* twist = out.mutable_twist();
  twist->set_linear_x(odom.twist.linear_x);
  twist->set_linear_y(odom.twist.linear_y);
  twist->set_angular_z(odom.twist.angular_z);

  std::string payload;
  out.SerializeToString(&payload);

  auto st = channel->log(
    reinterpret_cast<const std::byte*>(payload.data()),
    payload.size(),
    odom.ts.realtime.to_ns());

  if (st != foxglove::FoxgloveError::Ok) {
    std::cerr << "Failed to write odometry 2D record for " << odom.id << ": "
              << foxglove::strerror(st) << "\n";
  } else {
    ++stats_.odometry_2d_written;
  }
}

void TrossenMCAPBackend::write_image_record(const data::ImageRecord& img) {
  // Determine if this is a depth frame based on encoding or topic
  const bool depth =
    is_depth_encoding(img.encoding) || is_depth_topic(trossen_mcap_defs::image_topic(img.id));
  foxglove::RawChannel* channel = nullptr;
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
    channel = ensure_image_channel_with_metadata(img.id, md);
  } else {
    channel = ensure_image_channel(img.id);
  }
  if (!channel) {
    return;
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
  std::vector<uint8_t> payload(TROSSEN_MCAP_INITIAL_ENCODED_BUFFER_SIZE);
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

  auto st = channel->log(
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

  // Write optional depth image to a separate channel when ImageRecord carries depth
  if (img.has_depth()) {
    const std::string depth_topic_id = img.id + "_depth";
    std::unordered_map<std::string, std::string> md;
    md["stream_type"] = "depth";
    md["depth_encoding"] = "16UC1";
    if (img.depth_scale.has_value()) {
      md["depth_scale_m"] = std::to_string(img.depth_scale.value());
    }

    foxglove::RawChannel* depth_channel =
      ensure_image_channel_with_metadata(depth_topic_id, md);
    if (depth_channel) {
      foxglove::schemas::RawImage dmsg;
      dmsg.timestamp = foxglove::schemas::Timestamp{
        .sec = static_cast<uint32_t>(img.ts.realtime.sec),
        .nsec = static_cast<uint32_t>(img.ts.realtime.nsec)
      };
      dmsg.frame_id = depth_topic_id;
      dmsg.width = static_cast<uint32_t>(img.depth_image->cols);
      dmsg.height = static_cast<uint32_t>(img.depth_image->rows);
      dmsg.encoding = "16UC1";
      dmsg.step = static_cast<uint32_t>(img.depth_image->step);
      const std::byte* dptr =
        reinterpret_cast<const std::byte*>(img.depth_image->data);
      const size_t dsize = img.depth_image->total() * img.depth_image->elemSize();
      dmsg.data.assign(dptr, dptr + dsize);

      std::vector<uint8_t> dpayload(TROSSEN_MCAP_INITIAL_ENCODED_BUFFER_SIZE);
      size_t dencoded_len = 0;
      auto dencode_result = dmsg.encode(dpayload.data(), dpayload.size(), &dencoded_len);

      if (dencode_result == foxglove::FoxgloveError::BufferTooShort) {
        dpayload.resize(dencoded_len);
        dencode_result = dmsg.encode(dpayload.data(), dpayload.size(), &dencoded_len);
      }

      if (dencode_result != foxglove::FoxgloveError::Ok) {
        std::cerr << "Failed to encode depth image: "
                  << foxglove::strerror(dencode_result) << "\n";
      } else {
        auto dst = depth_channel->log(
          reinterpret_cast<const std::byte*>(dpayload.data()),
          dencoded_len,
          img.ts.realtime.to_ns());
        if (dst != foxglove::FoxgloveError::Ok) {
          std::cerr << "Failed to write depth image: " << foxglove::strerror(dst) << "\n";
        } else {
          ++stats_.depth_images_written;
        }
      }
    }
  }
}

void TrossenMCAPBackend::register_schemas_once() {
  const auto* pool = google::protobuf::DescriptorPool::generated_pool();

  // Helper: build a self-contained FileDescriptorSet for one root .proto file
  auto build_schema_blob = [&](const char* proto_path) -> std::string {
    google::protobuf::FileDescriptorSet set;
    std::unordered_set<std::string> visited;

    std::function<void(const google::protobuf::FileDescriptor*)> add_with_deps;
    add_with_deps = [&](const google::protobuf::FileDescriptor* fd) {
      if (!fd || !visited.insert(fd->name()).second) {
        return;
      }
      for (int i = 0; i < fd->dependency_count(); ++i) {
        add_with_deps(fd->dependency(i));
      }
      fd->CopyTo(set.add_file());
    };

    const google::protobuf::FileDescriptor* fd = pool->FindFileByName(proto_path);
    if (!fd) {
      // Basename fallback: compiler may have stripped the directory prefix
      std::string base(proto_path);
      if (auto pos = base.find_last_of('/'); pos != std::string::npos) {
        base = base.substr(pos + 1);
      }
      fd = pool->FindFileByName(base);
    }
    add_with_deps(fd);

    std::string blob;
    set.SerializeToString(&blob);
    return blob;
  };

  schema_data_js_ = build_schema_blob(
    "trossen_sdk/io/backends/trossen_mcap/proto/JointState.proto");
  schema_data_odom2d_ = build_schema_blob(
    "trossen_sdk/io/backends/trossen_mcap/proto/Odometry2D.proto");
}

bool TrossenMCAPBackend::is_depth_topic(const std::string& topic) {
  // Simple heuristic: contains "/depth/" before final name
  return topic.find("/depth/") != std::string::npos;
}

bool TrossenMCAPBackend::is_depth_encoding(const std::string& enc) {
  return enc == "depth16" || enc == "32FC1" || enc == "16UC1";  // allow alias
}


uint32_t TrossenMCAPBackend::scan_existing_episodes() {
  std::filesystem::path base_path = std::filesystem::path(cfg_->root) / cfg_->dataset_id;
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
