#include <chrono>
#include <unordered_set>
#include <iostream>

#include "mcap/types.hpp"
#include "opencv2/imgcodecs.hpp"

#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/io/backends/mcap/mcap_backend.hpp"
#include "trossen_sdk/version.hpp"
#include "JointState.pb.h"
#include "RawImage.pb.h"
#include "SessionMetadata.pb.h"


namespace trossen::io::backends {

McapBackend::McapBackend(Config cfg)
  : io::Backend(cfg.output_path), cfg_(std::move(cfg)) {}
McapBackend::~McapBackend() { close(); }

bool McapBackend::open() {
  std::scoped_lock lk(writer_mutex_);

  // Early return if already opened
  if (opened_) {
    return true;
  }

  // Parse configs
  path_ = cfg_.output_path;
  opts_.chunkSize = cfg_.chunk_size_bytes;
  if (cfg_.compression == "zstd") {
    opts_.compression = mcap::Compression::Zstd;
  } else if (cfg_.compression == "lz4") {
    opts_.compression = mcap::Compression::Lz4;
  } else if (cfg_.compression.empty()) {
    opts_.compression = mcap::Compression::None;
  } else {
    std::cerr << "Unknown compression option: " << cfg_.compression << " (falling back to none)\n";
    opts_.compression = mcap::Compression::None;
  }

  // Open mcap writer
  auto status = writer_.open(path_.string(), opts_);
  if (!status.ok()) {
    std::cerr << "Failed to open MCAP file: " << status.message << "\n";
    return false;
  }
  opened_ = true;

  register_schemas_once();
  write_sessionmetadata_once();
  return true;
}

void McapBackend::close() {
  std::scoped_lock lk(writer_mutex_);
  if (!opened_) {
    return;
  }
  writer_.close();
  opened_ = false;
}

void McapBackend::flush() {
  std::scoped_lock lk(writer_mutex_);
  if (!opened_) {
    return;
  }
  writer_.closeLastChunk();
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

void McapBackend::writeBatch(std::span<const data::RecordBase* const> records) {
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

void McapBackend::ensure_sessionmetadata_channel() {
  // Check if the channel already exists
  if (session_meta_channel_.id != 0) {
    return;
  }

  // Create a new channel
  mcap::Channel ch;
  ch.topic = mcapdefs::session_meta_topic();
  ch.messageEncoding = "protobuf";
  ch.schemaId = schema_session_;
  writer_.addChannel(ch);
  session_meta_channel_.id = ch.id;
  session_meta_channel_.topic = ch.topic;
  session_meta_channel_.encoding = ch.messageEncoding;
  session_meta_channel_.schema_id = ch.schemaId;
}

void McapBackend::write_sessionmetadata_once() {
  // Early return if already written
  if (session_meta_written_) {
    return;
  }

  // Create and write session metadata
  ensure_sessionmetadata_channel();
  trossen_sdk::msg::SessionMetadata sm;
  sm.set_run_id("RUN"); // TODO(lukeschmitt-tr): Generate UUID
  auto* ts = sm.mutable_timestamp();
  auto now = trossen::data::now_real();
  ts->set_seconds(now.sec);
  ts->set_nanos(now.nsec);
  sm.set_tool_version(trossen::core::version());
  sm.set_dataset_id(cfg_.dataset_id);
  sm.set_episode_index(cfg_.episode_index);
  std::string payload;
  sm.SerializeToString(&payload);
  mcap::Message msg;
  msg.channelId = session_meta_channel_.id;
  msg.sequence = 0;
  msg.logTime = now.to_ns();
  msg.publishTime = msg.logTime;
  msg.data = reinterpret_cast<const std::byte*>(payload.data());
  msg.dataSize = payload.size();
  auto st = writer_.write(msg);
  if (!st.ok()) {
    std::cerr << "Failed to write session meta: " << st.message << "\n";
  }
  // We have written the session metadata
  session_meta_written_ = true;
}

void McapBackend::ensure_jointstate_channel() {
  // Check if the channel already exists
  if (joint_channel_.id != 0) {
    return;
  }

  // Create a new channel
  mcap::Channel ch;
  ch.topic = mcapdefs::joint_state_topic(cfg_.robot_name);
  ch.messageEncoding = "protobuf";
  ch.schemaId = schema_joint_;
  writer_.addChannel(ch);
  joint_channel_.id = ch.id;
  joint_channel_.topic = ch.topic;
  joint_channel_.encoding = ch.messageEncoding;
  joint_channel_.schema_id = ch.schemaId;
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
    return; // already exists
  }
  mcap::Channel ch;
  ch.topic = mcapdefs::image_topic(camera_name);
  ch.messageEncoding = "protobuf";
  ch.schemaId = schema_image_;
  // Insert metadata (MCAP Channel supports key/value map)
  for (const auto& kv : metadata) {
    ch.metadata[kv.first] = kv.second;
  }
  writer_.addChannel(ch);
  ChannelInfo info;
  info.id = ch.id;
  info.topic = ch.topic;
  info.encoding = ch.messageEncoding;
  info.schema_id = ch.schemaId;
  image_channels_.emplace(camera_name, info);
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
  mcap::Message msg;
  msg.channelId = joint_channel_.id;
  msg.sequence = js.seq;
  msg.publishTime = js.ts.realtime.to_ns();
  msg.logTime = js.ts.realtime.to_ns();
  msg.data = reinterpret_cast<const std::byte*>(payload.data());
  msg.dataSize = payload.size();
  auto st = writer_.write(msg);
  if (!st.ok()) {
    std::cerr << "Failed to write joint state: " << st.message << "\n";
  }
  else {
    ++stats_.joint_states_written;
  }
}

void McapBackend::write_image_record(const data::ImageRecord& img) {
  // Determine if this is a depth frame based on encoding or topic
  const bool depth = is_depth_encoding(img.encoding) || is_depth_topic(mcapdefs::image_topic(img.id));
  if (depth) {
    // Depth metadata; attempt to parse scale if provided in encoding (future) - for now leave blank
    // We cannot know depth_scale_m here without augmenting ImageRecord; future extension could pass via id pattern.
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
  foxglove::RawImage imsg;
  auto* ts = imsg.mutable_timestamp();
  ts->set_seconds(img.ts.realtime.sec);
  ts->set_nanos(img.ts.realtime.nsec);
  imsg.set_frame_id(img.id);
  imsg.set_width(img.width);
  imsg.set_height(img.height);
  imsg.set_encoding(img.encoding);
  // Step: use OpenCV stride (bytes per row) if available
  imsg.set_step(static_cast<uint32_t>(img.image.step));
  imsg.set_data(
    reinterpret_cast<const char*>(img.image.data),
    img.image.total() * img.image.elemSize());
  std::string payload;
  imsg.SerializeToString(&payload);
  mcap::Message msg;
  msg.channelId = image_channels_.at(img.id).id;
  msg.sequence = img.seq;
  msg.logTime = img.ts.realtime.to_ns();
  msg.publishTime = msg.logTime;
  msg.data = reinterpret_cast<const std::byte*>(payload.data());
  msg.dataSize = payload.size();
  auto st = writer_.write(msg);
  if (!st.ok()) {
    std::cerr << "Failed to write image: " << st.message << "\n";
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
    "trossen_sdk/io/backends/mcap/proto/Rawimage.proto",
    "trossen_sdk/io/backends/mcap/proto/SessionMetadata.proto",
    "trossen_sdk/io/backends/mcap/proto/Timestamp.proto"
  };

  std::unordered_set<std::string> visited;
  std::function<void(const google::protobuf::FileDescriptor*)> add_with_deps;
  add_with_deps = [&](const google::protobuf::FileDescriptor* fd) {
    if (!fd) return;
    if (!visited.insert(fd->name()).second) return; // already added
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

  // Register schemas we care about and store IDs
  auto addSchema = [&](const std::string& name) -> mcap::SchemaId {
    mcap::Schema s;
    s.name = name;
    s.encoding = "protobuf";
    s.data.assign(reinterpret_cast<const std::byte*>(blob.data()), reinterpret_cast<const std::byte*>(blob.data() + blob.size()));
    writer_.addSchema(s);
    return s.id;
  };
  schema_session_ = addSchema("trossen_sdk.msg.SessionMetadata");
  schema_joint_   = addSchema("trossen_sdk.msg.JointState");
  schema_image_   = addSchema("foxglove.RawImage");
}

bool McapBackend::is_depth_topic(const std::string& topic) {
  // Simple heuristic: contains "/depth/" before final name
  return topic.find("/depth/") != std::string::npos;
}

bool McapBackend::is_depth_encoding(const std::string& enc) {
  return enc == "depth16" || enc == "32FC1" || enc == "16UC1"; // allow alias
}

} // namespace trossen::io::backends
