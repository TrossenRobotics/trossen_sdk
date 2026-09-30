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
#include "opencv2/imgproc.hpp"

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
  if (!cfg_->task_description.empty()) {
    std::cout << "Task Description: " << cfg_->task_description << std::endl;
  }
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

  // Validate the requested camera format before creating anything. A typo, or a
  // video request against a build without the encoder, fails the episode here
  // rather than silently writing raw frames, which would only surface much
  // later, at conversion time, as a dataset nobody asked for.
  if (!cfg_->image_encoding_is_valid()) {
    std::cerr << "Unknown image_encoding \"" << cfg_->image_encoding << "\" (expected \""
              << trossen::configuration::TROSSEN_MCAP_IMAGE_ENCODING_RAW << "\" or \""
              << trossen::configuration::TROSSEN_MCAP_IMAGE_ENCODING_VIDEO << "\")\n";
    return false;
  }
#ifndef TROSSEN_ENABLE_VIDEO_ENCODE
  if (cfg_->records_video()) {
    std::cerr << "image_encoding is \"video\" but this SDK was built without "
                 "TROSSEN_ENABLE_VIDEO_ENCODE; rebuild with "
                 "-DTROSSEN_ENABLE_VIDEO_ENCODE=ON or set image_encoding to \"raw\"\n";
    return false;
  }
#endif

  const std::filesystem::path dataset_dir = std::filesystem::path(cfg_->root) / cfg_->dataset_id;
  do {
    path_ = dataset_dir / (trossen::io::backends::generate_episode_id() + ".mcap");
  } while (std::filesystem::exists(path_));

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
  // Record the episode's id (the filename without the ".mcap" extension) so a file's
  // identity is queryable from metadata, not only its name.
  metadata["episode_id"] = path_.stem().string();
  auto now = trossen::data::now_real();
  metadata["recording_start_time"] = std::to_string(now.to_ns());
  if (!cfg_->task_description.empty()) {
    metadata["task_description"] = cfg_->task_description;
  }

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

void TrossenMCAPBackend::close_resources() {
  // Caller must hold writer_mutex_. Closes channels and writer.
  if (!opened_) return;

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

  // Encoders hold per-stream state (reference frames, GOP position), so they
  // must not survive into the next episode: a stream that begins mid-GOP is not
  // independently decodable, which is exactly what a per-episode file has to be.
  video_encoders_.clear();
  video_encode_failed_.clear();
  opened_ = false;
}

void TrossenMCAPBackend::close() {
  std::scoped_lock lk(writer_mutex_);
  close_resources();
}

void TrossenMCAPBackend::discard_episode() {
  std::scoped_lock lk(writer_mutex_);
  close_resources();

  // Determine which file to delete (works even if already closed by the sink).
  //
  // Filenames are UUIDs, so the file cannot be reconstructed from an index.
  // A live backend that called open() knows its own file via path_. The re-record path
  // (SessionManager::discard_last_episode) instead spins up a fresh backend that never
  // opened a file, so path_ is empty; there we delete the most-recently-written episode,
  // which is the just-finished one this local operation is meant to discard.
  std::filesystem::path target = path_;
  if (target.empty()) {
    target = find_latest_episode_file();
  }

  if (target.empty()) {
    std::cerr << "Warning: No MCAP episode file found to discard.\n";
    return;
  }

  try {
    std::filesystem::remove(target);
  } catch (const std::filesystem::filesystem_error& e) {
    std::cerr << "Warning: Failed to remove MCAP file during discard: "
              << e.what() << "\n";
  }
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

  // The schema has to match what will actually be logged on this channel, so it
  // follows the configured storage format rather than being fixed to RawImage.
  const bool video = cfg_ && cfg_->records_video();
  foxglove::Schema schema =
      video ? foxglove::schemas::CompressedVideo::schema() : foxglove::schemas::RawImage::schema();

  // Convert metadata to std::map
  std::map<std::string, std::string> channel_metadata(metadata.begin(), metadata.end());
  if (video) {
    // Recorded in the channel metadata so a reader can tell which codec a
    // stream carries without decoding a packet to find out. Depth is HEVC
    // because it needs 12-bit; color is H.264.
    const auto stream_type = channel_metadata.find("stream_type");
    const bool depth_stream =
        stream_type != channel_metadata.end() && stream_type->second == "depth";
    channel_metadata["video_format"] = depth_stream ? "h265" : "h264";
  }

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

void TrossenMCAPBackend::write_raw_image_message(const cv::Mat& image, const std::string& frame_id,
                                                 uint32_t width, uint32_t height,
                                                 const std::string& encoding,
                                                 const data::Timespec& ts,
                                                 foxglove::RawChannel* channel, uint64_t* counter) {
  foxglove::schemas::RawImage msg;
  msg.timestamp = foxglove::schemas::Timestamp{.sec = static_cast<uint32_t>(ts.sec),
                                               .nsec = static_cast<uint32_t>(ts.nsec)};
  msg.frame_id = frame_id;
  msg.width = width;
  msg.height = height;
  msg.encoding = encoding;
  msg.step = static_cast<uint32_t>(image.step);
  // Copy image data to std::vector<std::byte>
  const std::byte* data_ptr = reinterpret_cast<const std::byte*>(image.data);
  const size_t data_size = image.total() * image.elemSize();
  msg.data.assign(data_ptr, data_ptr + data_size);

  // Encode to buffer
  std::vector<uint8_t> payload(TROSSEN_MCAP_INITIAL_ENCODED_BUFFER_SIZE);
  size_t encoded_len = 0;
  auto encode_result = msg.encode(payload.data(), payload.size(), &encoded_len);
  if (encode_result == foxglove::FoxgloveError::BufferTooShort) {
    // Resize and try again
    payload.resize(encoded_len);
    encode_result = msg.encode(payload.data(), payload.size(), &encoded_len);
  }
  if (encode_result != foxglove::FoxgloveError::Ok) {
    std::cerr << "Failed to encode image for " << frame_id << ": "
              << foxglove::strerror(encode_result) << "\n";
    return;
  }

  auto st =
      channel->log(reinterpret_cast<const std::byte*>(payload.data()), encoded_len, ts.to_ns());
  if (st != foxglove::FoxgloveError::Ok) {
    std::cerr << "Failed to write image for " << frame_id << ": " << foxglove::strerror(st) << "\n";
  } else {
    ++(*counter);
  }
}

utils::VideoEncoder* TrossenMCAPBackend::ensure_video_encoder(const data::ImageRecord& img,
                                                              bool depth) {
#ifdef TROSSEN_ENABLE_VIDEO_ENCODE
  auto it = video_encoders_.find(img.id);
  if (it != video_encoders_.end()) {
    return it->second.get();
  }

  utils::VideoEncoder::Params p;
  p.width = static_cast<int>(img.width);
  p.height = static_cast<int>(img.height);
  // Nominal rate, for the encoder's stream time base only. True frame timing
  // lives in each message's MCAP log time, the capture stamp, which is what
  // the converter aligns on, so this need not match the camera's actual rate.
  p.fps = trossen::configuration::TROSSEN_MCAP_VIDEO_NOMINAL_FPS;
  p.gop_size = cfg_->video_keyframe_interval;
  p.encoder = cfg_->video_encoder;
  if (depth) {
    // 12-bit depth codes cannot survive an 8-bit codec, and quantization has
    // already discarded everything that can be spared, so encode losslessly.
    p.codec = utils::VideoCodec::H265;
    p.lossless = true;
  } else {
    p.codec = utils::VideoCodec::H264;
    p.bitrate_kbps = cfg_->video_bitrate_kbps;
  }

  auto encoder = utils::VideoEncoder::create(p);
  if (!encoder) {
    std::cerr << "Failed to create video encoder for camera " << img.id << "\n";
    return nullptr;
  }
  auto [inserted, _] = video_encoders_.emplace(img.id, std::move(encoder));
  return inserted->second.get();
#else
  (void)img;
  (void)depth;
  return nullptr;
#endif
}

void TrossenMCAPBackend::write_video_frame(const data::ImageRecord& img, bool depth,
                                           foxglove::RawChannel* channel) {
#ifdef TROSSEN_ENABLE_VIDEO_ENCODE
  auto* encoder = ensure_video_encoder(img, depth);
  if (!encoder) {
    if (!video_encode_failed_[img.id]) {
      video_encode_failed_[img.id] = true;
      std::cerr << "Dropping frames for " << img.id << ": no video encoder\n";
    }
    return;
  }

  // Color is handed over as BGR8; depth is log-quantized to 12-bit codes first
  // so the stored stream is exactly what LeRobot's depth decoder expects.
  cv::Mat source;
  if (depth) {
    if (img.image.type() != CV_16UC1) {
      if (!video_encode_failed_[img.id]) {
        video_encode_failed_[img.id] = true;
        std::cerr << "Depth video for " << img.id << " needs CV_16UC1, got type "
                  << img.image.type() << "\n";
      }
      return;
    }
    if (depth_quant_lut_.empty()) {
      depth_quant_lut_ = utils::build_depth_quantization_lut();
    }
    source.create(img.image.rows, img.image.cols, CV_16UC1);
    for (int y = 0; y < img.image.rows; ++y) {
      const uint16_t* src = img.image.ptr<uint16_t>(y);
      uint16_t* dst = source.ptr<uint16_t>(y);
      for (int x = 0; x < img.image.cols; ++x) dst[x] = depth_quant_lut_[src[x]];
    }
  } else {
    // The encoder converts BGR to YUV itself, so anything else is normalized
    // here, otherwise the red and blue channels swap silently.
    if (img.encoding == "rgb8") {
      cv::cvtColor(img.image, source, cv::COLOR_RGB2BGR);
    } else if (img.encoding == "rgba8") {
      cv::cvtColor(img.image, source, cv::COLOR_RGBA2BGR);
    } else if (img.encoding == "bgra8") {
      cv::cvtColor(img.image, source, cv::COLOR_BGRA2BGR);
    } else if (img.image.type() == CV_8UC1) {
      cv::cvtColor(img.image, source, cv::COLOR_GRAY2BGR);
    } else {
      source = img.image;
    }
  }

  const auto* src_data = reinterpret_cast<const uint8_t*>(source.data);
  const size_t src_size = source.total() * source.elemSize();
  const utils::VideoEncoder::EncodedFrame packet = encoder->encode(src_data, src_size);
  if (packet.data.empty()) {
    // One packet per frame is an invariant, not a nicety: the converter pairs
    // camera frames to joint samples, so a dropped packet shifts every later
    // frame's alignment. Report it rather than letting it pass quietly.
    if (!video_encode_failed_[img.id]) {
      video_encode_failed_[img.id] = true;
      std::cerr << "Video encoder produced no packet for " << img.id
                << "; frame alignment would drift, dropping frame\n";
    }
    return;
  }

  foxglove::schemas::CompressedVideo vmsg;
  vmsg.timestamp =
      foxglove::schemas::Timestamp{.sec = static_cast<uint32_t>(img.ts.realtime.sec),
                                   .nsec = static_cast<uint32_t>(img.ts.realtime.nsec)};
  vmsg.frame_id = img.id;
  vmsg.format = utils::video_codec_format(encoder->codec());
  vmsg.data.assign(packet.data.begin(), packet.data.end());

  std::vector<uint8_t> payload(TROSSEN_MCAP_INITIAL_ENCODED_BUFFER_SIZE);
  size_t encoded_len = 0;
  auto encode_result = vmsg.encode(payload.data(), payload.size(), &encoded_len);
  if (encode_result == foxglove::FoxgloveError::BufferTooShort) {
    payload.resize(encoded_len);
    encode_result = vmsg.encode(payload.data(), payload.size(), &encoded_len);
  }
  if (encode_result != foxglove::FoxgloveError::Ok) {
    std::cerr << "Failed to encode CompressedVideo: " << foxglove::strerror(encode_result) << "\n";
    return;
  }

  auto st = channel->log(reinterpret_cast<const std::byte*>(payload.data()), encoded_len,
                         img.ts.realtime.to_ns());
  if (st != foxglove::FoxgloveError::Ok) {
    std::cerr << "Failed to write video frame: " << foxglove::strerror(st) << "\n";
  } else {
    if (depth) {
      ++stats_.depth_images_written;
    } else {
      ++stats_.images_written;
    }
  }
#else
  (void)img;
  (void)depth;
  (void)channel;
#endif
}

void TrossenMCAPBackend::write_image_frame(const data::ImageRecord& img, bool depth,
                                           foxglove::RawChannel* channel) {
  if (cfg_ && cfg_->records_video()) {
    write_video_frame(img, depth, channel);
    return;
  }
  write_raw_image_message(img.image, img.id, img.width, img.height, img.encoding, img.ts.realtime,
                          channel, depth ? &stats_.depth_images_written : &stats_.images_written);
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

  write_image_frame(img, depth, channel);

  // Write optional depth image to a separate channel when ImageRecord carries depth
  if (img.has_depth()) {
    const std::string depth_topic_id = img.id + "_depth";
    std::unordered_map<std::string, std::string> md;
    md["stream_type"] = "depth";
    md["depth_encoding"] = "16UC1";
    if (img.depth_scale.has_value()) {
      md["depth_scale_m"] = std::to_string(img.depth_scale.value());
    }

    foxglove::RawChannel* depth_channel = ensure_image_channel_with_metadata(depth_topic_id, md);
    if (depth_channel) {
      // Build a lightweight ImageRecord view over the depth plane so it gets
      // its own encoder/channel; color and depth must never share either.
      data::ImageRecord drec = img;
      drec.id = depth_topic_id;
      drec.image = *img.depth_image;
      drec.encoding = "16UC1";
      drec.width = static_cast<uint32_t>(img.depth_image->cols);
      drec.height = static_cast<uint32_t>(img.depth_image->rows);
      write_image_frame(drec, /*depth=*/true, depth_channel);
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


// Pattern for episode filenames: a canonical UUID (the current <uuid>.mcap naming) or the
// legacy zero-padded episode_NNNNNN.mcap, so resuming/gating/discarding still see episodes
// written by older SDKs.
static const std::regex& episode_filename_pattern() {
  // TODO(lukeschmitt-tr): remove the legacy pattern once no longer needed.
  static const std::regex pattern(
    R"(([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}|episode_[0-9]{6})\.mcap)");
  return pattern;
}

std::filesystem::path TrossenMCAPBackend::find_latest_episode_file() const {
  // "Latest" is decided by last-write time.
  std::filesystem::path base_path = std::filesystem::path(cfg_->root) / cfg_->dataset_id;
  if (!std::filesystem::is_directory(base_path)) {
    return {};
  }

  std::filesystem::path latest;
  std::filesystem::file_time_type latest_time{};
  try {
    for (const auto& entry : std::filesystem::directory_iterator(base_path)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      if (!std::regex_match(entry.path().filename().string(), episode_filename_pattern())) {
        continue;
      }
      auto mtime = entry.last_write_time();
      if (latest.empty() || mtime > latest_time) {
        latest = entry.path();
        latest_time = mtime;
      }
    }
  } catch (const std::filesystem::filesystem_error& e) {
    std::cerr << "Filesystem error while locating latest episode: " << e.what() << std::endl;
    return {};
  }
  return latest;
}

uint32_t TrossenMCAPBackend::scan_existing_episodes() {
  std::filesystem::path base_path = std::filesystem::path(cfg_->root) / cfg_->dataset_id;
  // If directory doesn't exist, there are no episodes yet
  if (!std::filesystem::exists(base_path)) {
    return 0;
  }

  // If not a directory, return 0
  if (!std::filesystem::is_directory(base_path)) {
    std::cerr << "Warning: base_path exists but is not a directory: " << base_path << std::endl;
    return 0;
  }

  // Filenames are UUIDs, so there is no index to parse. Count the existing
  // episode files; SessionManager uses this count to resume and to enforce max_episodes.
  uint32_t count = 0;
  try {
    for (const auto& entry : std::filesystem::directory_iterator(base_path)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      if (std::regex_match(entry.path().filename().string(), episode_filename_pattern())) {
        ++count;
      }
      // Silently ignore non-episode files (as per design doc)
    }
  } catch (const std::filesystem::filesystem_error& e) {
    std::cerr << "Filesystem error while scanning episodes: " << e.what() << std::endl;
    return 0;
  }

  return count;
}

}  // namespace trossen::io::backends
