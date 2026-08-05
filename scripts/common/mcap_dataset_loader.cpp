/**
 * @file mcap_dataset_loader.cpp
 * @brief Implementation of the shared MCAP read + alignment logic.
 *
 * This translation unit owns the single MCAP_IMPLEMENTATION definition for the
 * converter tools; executables link against it without defining the macro.
 */

#define MCAP_IMPLEMENTATION
#include "mcap_dataset_loader.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <regex>
#include <string_view>

#include <opencv2/opencv.hpp>

#include "JointState.pb.h"
#include "Odometry2D.pb.h"
#include "CompressedVideo.pb.h"
#include "RawImage.pb.h"

namespace trossen::convert {

namespace {

/// @brief Nearest-sample matching tolerance: streams further than this are treated as missing.
constexpr uint64_t kToleranceNs = 50000000;  // 50 ms

/// @brief Dataset frame rate. Forced to a fixed value for deterministic stream sync.
constexpr double kFps = 30.0;

void on_problem(const mcap::Status& problem) {
  std::cerr << "Warning: MCAP parsing issue: " << problem.message << "\n";
}

/// @brief Native lerobot_trossen camera key: replace the `camera_` prefix with `cam_`.
std::string native_camera_key(const std::string& name) {
  constexpr std::string_view kPrefix = "camera_";
  if (name.rfind(kPrefix, 0) == 0) {
    return "cam_" + name.substr(kPrefix.size());
  }
  return name;
}

/// @brief Arm side of a joint stream: `follower_left`/`leader_left` → `left`. Empty if no side.
std::string stream_side(const std::string& stream_id) {
  for (std::string_view p : {"follower_", "leader_"}) {
    if (stream_id.rfind(p, 0) == 0) return stream_id.substr(p.size());
  }
  return stream_id;
}

/// @brief True if a camera is a depth map (dataset_info flag or a `_depth` name suffix).
bool camera_is_depth(const std::string& camera_name, const nlohmann::json& dataset_info) {
  if (!dataset_info.empty() && dataset_info.contains("cameras") &&
      dataset_info["cameras"].contains(camera_name) &&
      dataset_info["cameras"][camera_name].value("is_depth_map", false)) {
    return true;
  }
  constexpr std::string_view kSuffix = "_depth";
  return camera_name.size() >= kSuffix.size() &&
         camera_name.compare(camera_name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

}  // namespace

bool load_aligned_episode(
  const std::string& mcap_file,
  int episode_index,
  AlignedEpisode& out,
  McapChannelMap& channels,
  bool native_schema)
{
  out = AlignedEpisode{};
  channels = McapChannelMap{};
  out.episode_index = episode_index;
  out.fps = static_cast<float>(kFps);

  std::ifstream input(mcap_file, std::ios::binary);
  if (!input.is_open()) {
    std::cerr << "Error: Failed to open MCAP file\n";
    return false;
  }

  mcap::McapReader reader;
  auto status = reader.open(input);
  if (!status.ok()) {
    std::cerr << "Error: Failed to parse MCAP file: " << status.message << "\n";
    return false;
  }

  auto summary_status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
  if (!summary_status.ok()) {
    std::cerr << "Error: Failed to read MCAP summary: " << summary_status.message << "\n";
    return false;
  }

  // ── PHASE 1: Extract embedded dataset_info metadata (joint names, camera specs) ──
  auto* data_source = reader.dataSource();
  const auto& meta_indexes = reader.metadataIndexes();
  auto range = meta_indexes.equal_range("trossen_sdk_recording");
  for (auto it = range.first; it != range.second; ++it) {
    mcap::Record raw_record;
    auto rs = mcap::McapReader::ReadRecord(*data_source, it->second.offset, &raw_record);
    if (!rs.ok()) continue;

    mcap::Metadata meta_record;
    rs = mcap::McapReader::ParseMetadata(raw_record, &meta_record);
    if (!rs.ok()) continue;

    auto info_it = meta_record.metadata.find("dataset_info");
    if (info_it != meta_record.metadata.end()) {
      try {
        out.mcap_dataset_info = nlohmann::json::parse(info_it->second);
        std::cout << "  [ok] Found MCAP dataset_info metadata\n";
        if (out.mcap_dataset_info.contains("robot_name")) {
          out.robot_name = out.mcap_dataset_info["robot_name"].get<std::string>();
          std::cout << "    Robot name from MCAP: " << out.robot_name << "\n";
        }
        // Per-episode task prompt embedded by the recorder (see
        // trossen_mcap_backend.cpp). Left empty when the recording predates
        // task-embedding or the operator set no task; the converter falls back
        // to its configured task_name in that case.
        if (out.mcap_dataset_info.contains("task")) {
          out.task_name = out.mcap_dataset_info["task"].get<std::string>();
          if (!out.task_name.empty()) {
            std::cout << "    Task from MCAP: " << out.task_name << "\n";
          }
        }
      } catch (const std::exception& e) {
        std::cerr << "  Warning: Failed to parse dataset_info metadata: " << e.what() << "\n";
      }
    }
  }

  // ── PHASE 2: Map channels to streams; auto-detect leader/follower arms ──
  std::vector<std::string> detected_leader_streams;
  std::vector<std::string> detected_follower_streams;

  std::cout << "  Available channels:\n";
  for (const auto& [channel_id, channel_ptr] : reader.channels()) {
    std::string topic = channel_ptr->topic;
    std::cout << "    - Topic: '" << topic << "'\n";

    size_t odom_pos = topic.find("/odom/state");
    if (odom_pos != std::string::npos) {
      std::string stream_id = topic.substr(0, odom_pos);
      if (!stream_id.empty() && stream_id[0] == '/') {
        stream_id = stream_id.substr(1);
      }
      channels.slate_base_channel_id = channel_id;
      channels.has_slate_base = true;
      std::cout << "    [ok] Found odometry stream for mobile robot: " << stream_id << "\n";
      continue;
    }

    size_t pos = topic.find("/joints/state");
    if (pos != std::string::npos) {
      std::string stream_id = topic.substr(0, pos);
      if (!stream_id.empty() && stream_id[0] == '/') {
        stream_id = stream_id.substr(1);
      }
      channels.joint_channels[channel_id] = stream_id;

      if (stream_id.find("leader") != std::string::npos) {
        detected_leader_streams.push_back(stream_id);
        std::cout << "    [ok] Detected leader stream: " << stream_id << "\n";
      } else if (stream_id.find("follower") != std::string::npos) {
        detected_follower_streams.push_back(stream_id);
        std::cout << "    [ok] Detected follower stream: " << stream_id << "\n";
      }
    }

    // Topic format: /cameras/<camera_name>/image
    {
      static const std::regex camera_topic_re("^/cameras/(.+)/image$");
      std::smatch m;
      if (std::regex_match(topic, m, camera_topic_re)) {
        channels.camera_channels[channel_id] = m[1].str();
      }
    }
  }

  if (channels.joint_channels.empty()) {
    std::cerr << "Error: No joint state channels found in MCAP file\n";
    return false;
  }

  if (!detected_leader_streams.empty() && !detected_follower_streams.empty()) {
    std::sort(detected_leader_streams.begin(), detected_leader_streams.end());
    std::sort(detected_follower_streams.begin(), detected_follower_streams.end());
    out.leader_streams = detected_leader_streams;
    out.follower_streams = detected_follower_streams;
    std::cout << "\n  [ok] Auto-detected configuration:\n";
    std::cout << "    Leader streams (" << out.leader_streams.size() << "): ";
    for (const auto& s : out.leader_streams) std::cout << s << " ";
    std::cout << "\n    Follower streams (" << out.follower_streams.size() << "): ";
    for (const auto& s : out.follower_streams) std::cout << s << " ";
    std::cout << "\n";
  } else {
    // Fallback: single-robot mode — every non-base stream is both leader and follower.
    std::vector<std::string> all_streams;
    for (const auto& [channel_id, stream_id] : channels.joint_channels) {
      if (stream_id != "slate_base") {
        all_streams.push_back(stream_id);
      }
    }
    std::sort(all_streams.begin(), all_streams.end());
    all_streams.erase(std::unique(all_streams.begin(), all_streams.end()), all_streams.end());

    if (all_streams.empty()) {
      std::cerr << "Error: No usable joint state streams found\n";
      return false;
    }
    out.leader_streams = all_streams;
    out.follower_streams = all_streams;
    std::cout << "\n  [ok] Single robot mode detected " << all_streams.size()
              << " stream(s):\n    ";
    for (const auto& s : all_streams) std::cout << s << " ";
    std::cout << "\n";
  }

  if (!channels.camera_channels.empty()) {
    std::cout << "  Found " << channels.camera_channels.size() << " camera channel(s)\n";
  }

  // ── Parse all joint-state and odometry messages into per-stream buffers ──
  std::cout << "\nParsing joint state messages...\n";
  std::map<std::string, std::vector<JointStateMessage>> messages_by_stream;
  std::vector<Odometry2DMessage> slate_base_messages;
  std::map<std::string, size_t> camera_image_counts;

  size_t total_messages = 0;
  size_t total_images = 0;

  for (const auto& messageView : reader.readMessages(on_problem)) {
    if (channels.has_slate_base && messageView.channel->id == channels.slate_base_channel_id) {
      trossen_sdk::msg::Odometry2D odom_msg;
      if (!odom_msg.ParseFromArray(reinterpret_cast<const char*>(messageView.message.data),
                                   messageView.message.dataSize)) {
        std::cerr << "Warning: Failed to parse Odometry2D message\n";
        continue;
      }
      Odometry2DMessage msg;
      msg.timestamp_ns = messageView.message.logTime;
      msg.vel_x = static_cast<double>(odom_msg.twist().linear_x());
      msg.vel_theta = static_cast<double>(odom_msg.twist().angular_z());
      slate_base_messages.push_back(msg);
      ++total_messages;
      continue;
    }

    auto joint_it = channels.joint_channels.find(messageView.channel->id);
    if (joint_it != channels.joint_channels.end()) {
      const std::string& stream_id = joint_it->second;
      trossen_sdk::msg::JointState js_msg;
      if (!js_msg.ParseFromArray(reinterpret_cast<const char*>(messageView.message.data),
                                 messageView.message.dataSize)) {
        std::cerr << "Warning: Failed to parse message for " << stream_id << "\n";
        continue;
      }

      JointStateMessage msg;
      msg.timestamp_ns = messageView.message.logTime;
      msg.stream_id = stream_id;
      for (auto v : js_msg.positions()) msg.positions.push_back(static_cast<double>(v));
      for (auto v : js_msg.velocities()) msg.velocities.push_back(static_cast<double>(v));
      messages_by_stream[stream_id].push_back(msg);
      ++total_messages;
      continue;
    }

    auto camera_it = channels.camera_channels.find(messageView.channel->id);
    if (camera_it != channels.camera_channels.end()) {
      camera_image_counts[camera_it->second]++;
      ++total_images;
    }
  }

  std::cout << "  [ok] Parsed " << total_messages << " joint state messages\n";
  for (const auto& [stream_id, messages] : messages_by_stream) {
    std::cout << "    - " << stream_id << ": " << messages.size() << " messages\n";
  }
  if (channels.has_slate_base) {
    std::cout << "    - slate_base: " << slate_base_messages.size() << " messages (velocities)\n";
  }
  if (total_images > 0) {
    std::cout << "  [ok] Found " << total_images << " camera images\n";
  }

  // ── Compute action/observation dimensions from the detected streams ──
  out.joints_per_stream = 0;
  for (const auto& [stream_id, messages] : messages_by_stream) {
    if (!messages.empty()) {
      out.joints_per_stream = static_cast<int>(messages[0].positions.size());
      break;
    }
  }
  out.has_slate_base = channels.has_slate_base;
  out.action_dim = static_cast<int>(out.leader_streams.size()) * out.joints_per_stream;
  out.obs_dim = static_cast<int>(out.follower_streams.size()) * out.joints_per_stream;
  if (out.has_slate_base) {
    out.action_dim += 2;
    out.obs_dim += 2;
  }

  // ── Select the reference (master-clock) stream ──
  std::string reference_stream;
  for (const auto& stream : out.follower_streams) {
    auto it = messages_by_stream.find(stream);
    if (it != messages_by_stream.end() && !it->second.empty()) {
      reference_stream = stream;
      break;
    }
  }
  if (reference_stream.empty()) {
    for (const auto& [stream_id, msgs] : messages_by_stream) {
      if (!msgs.empty()) {
        reference_stream = stream_id;
        std::cout << "  Note: Using single-robot mode with stream: " << stream_id << "\n";
        out.leader_streams = {stream_id};
        out.follower_streams = {stream_id};
        break;
      }
    }
  }
  if (reference_stream.empty()) {
    std::cerr << "Error: No joint state streams found in MCAP file\n";
    return false;
  }

  const auto& reference_messages = messages_by_stream[reference_stream];
  std::cout << "  Using " << reference_stream << " as reference (" << reference_messages.size()
            << " messages)\n";

  // Cap rows at the smallest camera frame count so every row has a frame to pair with.
  size_t max_rows = reference_messages.size();
  if (!camera_image_counts.empty()) {
    size_t min_camera_frames = std::numeric_limits<size_t>::max();
    for (const auto& [camera_name, count] : camera_image_counts) {
      min_camera_frames = std::min(min_camera_frames, count);
    }
    max_rows = std::min(max_rows, min_camera_frames);
    std::cout << "  Limiting to " << max_rows << " rows to match camera frame count\n";
  }

  // ── Align: for each reference timestamp, snap every stream to its nearest sample ──
  std::map<std::string, size_t> stream_indices;
  for (const auto& [stream_id, _] : messages_by_stream) {
    stream_indices[stream_id] = 0;
  }

  const double frame_duration_s = 1.0 / kFps;
  size_t slate_base_idx = 0;
  int64_t frame_index = 0;
  size_t rows_skipped = 0;

  auto find_closest_message = [&](const std::string& stream_id, uint64_t target_ts,
                                  size_t& idx) -> std::vector<double>* {
    auto it = messages_by_stream.find(stream_id);
    if (it == messages_by_stream.end() || it->second.empty()) return nullptr;
    const auto& messages = it->second;
    if (idx >= messages.size()) return nullptr;
    while (idx < messages.size() - 1 && messages[idx + 1].timestamp_ns <= target_ts) {
      ++idx;
    }
    if (std::abs(static_cast<int64_t>(messages[idx].timestamp_ns - target_ts)) >
        static_cast<int64_t>(kToleranceNs)) {
      return nullptr;
    }
    return const_cast<std::vector<double>*>(&messages[idx].positions);
  };

  out.frames.reserve(max_rows);
  for (size_t ref_idx = 0; ref_idx < max_rows; ++ref_idx) {
    const uint64_t timestamp_ns = reference_messages[ref_idx].timestamp_ns;

    std::vector<double> actions;
    bool have_all_leaders = true;
    for (const auto& leader_stream : out.leader_streams) {
      auto* positions = find_closest_message(leader_stream, timestamp_ns,
                                             stream_indices[leader_stream]);
      if (positions) {
        actions.insert(actions.end(), positions->begin(), positions->end());
      } else {
        have_all_leaders = false;
        break;
      }
    }

    std::vector<double> observations;
    bool have_all_followers = true;
    for (const auto& follower_stream : out.follower_streams) {
      auto* positions = find_closest_message(follower_stream, timestamp_ns,
                                             stream_indices[follower_stream]);
      if (positions) {
        observations.insert(observations.end(), positions->begin(), positions->end());
      } else {
        have_all_followers = false;
        break;
      }
    }

    std::vector<double> base_velocities;
    if (channels.has_slate_base) {
      while (slate_base_idx < slate_base_messages.size() - 1 &&
             slate_base_messages[slate_base_idx + 1].timestamp_ns <= timestamp_ns) {
        ++slate_base_idx;
      }
      if (slate_base_idx < slate_base_messages.size() &&
          std::abs(static_cast<int64_t>(
            slate_base_messages[slate_base_idx].timestamp_ns - timestamp_ns)) <=
            static_cast<int64_t>(kToleranceNs)) {
        const auto& odom = slate_base_messages[slate_base_idx];
        base_velocities.push_back(odom.vel_x);
        base_velocities.push_back(odom.vel_theta);
      }
      if (base_velocities.empty()) {
        base_velocities = {0.0, 0.0};
      }
    }

    if (!have_all_leaders || !have_all_followers) {
      ++rows_skipped;
      continue;
    }

    if (channels.has_slate_base) {
      actions.insert(actions.end(), base_velocities.begin(), base_velocities.end());
      observations.insert(observations.end(), base_velocities.begin(), base_velocities.end());
    }

    AlignedFrame frame;
    frame.timestamp_s = static_cast<float>(static_cast<double>(frame_index) * frame_duration_s);
    frame.action = std::move(actions);
    frame.observation = std::move(observations);
    out.frames.push_back(std::move(frame));
    ++frame_index;
  }

  std::cout << "  [ok] Aligned " << out.frames.size() << " frames";
  if (rows_skipped > 0) std::cout << " (skipped " << rows_skipped << " misaligned)";
  std::cout << "\n";

  // Record the cameras present (frame counts filled by extract_camera_images()).
  for (const auto& [channel_id, camera_name] : channels.camera_channels) {
    CameraInfo cam;
    cam.name = camera_name;  // original name; used for extraction dirs + dataset_info lookups
    const std::string out_name = native_schema ? native_camera_key(camera_name) : camera_name;
    cam.obs_key = "observation.images." + out_name;
    out.cameras.push_back(cam);
  }

  return true;
}

bool extract_camera_images(
  const std::string& mcap_file,
  const McapChannelMap& channels,
  const std::function<std::filesystem::path(const std::string& camera_name)>& dir_for,
  std::map<std::string, size_t>& out_counts,
  bool native_schema)
{
  namespace fs = std::filesystem;
  out_counts.clear();
  if (channels.camera_channels.empty()) {
    return true;
  }

  std::map<std::string, fs::path> camera_dirs;
  for (const auto& [channel_id, camera_name] : channels.camera_channels) {
    camera_dirs[camera_name] = dir_for(camera_name);
    out_counts[camera_name] = 0;
  }

  std::ifstream image_input(mcap_file, std::ios::binary);
  mcap::McapReader image_reader;
  auto img_status = image_reader.open(image_input);
  if (!img_status.ok()) {
    std::cerr << "Error: Failed to reopen MCAP file for images: " << img_status.message << "\n";
    return false;
  }
  auto img_summary_status = image_reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
  if (!img_summary_status.ok()) {
    std::cerr << "Error: Failed to read MCAP summary for images: " << img_summary_status.message
              << "\n";
    return false;
  }

  size_t images_saved = 0;
  for (const auto& messageView : image_reader.readMessages(on_problem)) {
    auto it = channels.camera_channels.find(messageView.channel->id);
    if (it == channels.camera_channels.end()) continue;
    // A recording may store some cameras as compressed video; those are handled
    // by extract_camera_video(). Skipping them by schema keeps this path from
    // reporting a parse failure per frame on a perfectly good recording.
    if (messageView.schema && messageView.schema->name == "foxglove.CompressedVideo") {
      continue;
    }
    const std::string& camera_name = it->second;
    size_t frame_idx = out_counts[camera_name];

    foxglove::RawImage raw_image;
    if (!raw_image.ParseFromArray(messageView.message.data,
                                  static_cast<int>(messageView.message.dataSize))) {
      std::cerr << "Warning: Failed to parse RawImage message for " << camera_name << " frame "
                << frame_idx << "\n";
      out_counts[camera_name]++;
      continue;
    }

    int cv_type = -1;
    if (raw_image.encoding() == "bgr8" || raw_image.encoding() == "8UC3") {
      cv_type = CV_8UC3;
    } else if (raw_image.encoding() == "rgb8") {
      cv_type = CV_8UC3;
    } else if (raw_image.encoding() == "rgba8") {
      cv_type = CV_8UC4;
    } else if (raw_image.encoding() == "bgra8") {
      cv_type = CV_8UC4;
    } else if (raw_image.encoding() == "mono8" || raw_image.encoding() == "8UC1") {
      cv_type = CV_8UC1;
    } else if (raw_image.encoding() == "mono16" || raw_image.encoding() == "16UC1") {
      cv_type = CV_16UC1;
    } else if (raw_image.encoding() == "32FC1") {
      cv_type = CV_32FC1;
    } else {
      std::cerr << "Warning: Unsupported encoding '" << raw_image.encoding() << "' for "
                << camera_name << " frame " << frame_idx << "\n";
      out_counts[camera_name]++;
      continue;
    }

    cv::Mat image(raw_image.height(), raw_image.width(), cv_type,
                  const_cast<char*>(raw_image.data().data()), raw_image.step());

    if (raw_image.encoding() == "rgb8") {
      cv::cvtColor(image, image, cv::COLOR_RGB2BGR);
    } else if (raw_image.encoding() == "rgba8") {
      cv::cvtColor(image, image, cv::COLOR_RGBA2BGR);
    } else if (raw_image.encoding() == "bgra8") {
      cv::cvtColor(image, image, cv::COLOR_BGRA2BGR);
    }

    cv::Mat image_copy = image.clone();
    if (image_copy.empty()) {
      std::cerr << "Warning: Empty image for " << camera_name << " frame " << frame_idx << "\n";
      out_counts[camera_name]++;
      continue;
    }

    // Native schema preserves 16-bit depth losslessly as PNG (JPEG is 8-bit and would
    // destroy the depth). RGB/8-bit stays JPEG. The writer picks its encode path by the
    // frame extension it finds (.png → gray12le HEVC depth, .jpg → av1 RGB).
    const bool depth_png = native_schema && cv_type == CV_16UC1;
    char namebuf[32];
    std::snprintf(namebuf, sizeof(namebuf), depth_png ? "image_%06zu.png" : "image_%06zu.jpg",
                  frame_idx);
    fs::path image_path = camera_dirs[camera_name] / namebuf;

    std::vector<int> compression_params =
      depth_png ? std::vector<int>{cv::IMWRITE_PNG_COMPRESSION, 1}
                : std::vector<int>{cv::IMWRITE_JPEG_QUALITY, 95};
    if (cv::imwrite(image_path.string(), image_copy, compression_params)) {
      ++images_saved;
      out_counts[camera_name]++;
    } else {
      std::cerr << "Warning: Failed to save image: " << image_path.string() << "\n";
      out_counts[camera_name]++;
    }
  }

  std::cout << "  [ok] Saved " << images_saved << " images\n";
  for (const auto& [camera_name, count] : out_counts) {
    std::cout << "    - " << camera_name << ": " << count << " images\n";
  }
  return true;
}

bool extract_camera_video(
  const std::string& mcap_file,
  const McapChannelMap& channels,
  const std::function<std::filesystem::path(const std::string& camera_name)>& dir_for,
  std::map<std::string, CameraVideoStream>& out_streams)
{
  namespace fs = std::filesystem;
  out_streams.clear();
  if (channels.camera_channels.empty()) {
    return true;
  }

  std::ifstream input(mcap_file, std::ios::binary);
  mcap::McapReader reader;
  auto status = reader.open(input);
  if (!status.ok()) {
    std::cerr << "Error: Failed to reopen MCAP file for video: " << status.message << "\n";
    return false;
  }
  auto summary_status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
  if (!summary_status.ok()) {
    std::cerr << "Error: Failed to read MCAP summary for video: " << summary_status.message << "\n";
    return false;
  }

  // Opened lazily so a recording with no video channels creates no files.
  std::map<std::string, std::ofstream> outputs;

  for (const auto& messageView : reader.readMessages(on_problem)) {
    auto it = channels.camera_channels.find(messageView.channel->id);
    if (it == channels.camera_channels.end()) continue;
    // Only video channels here; RawImage cameras belong to extract_camera_images().
    if (!messageView.schema || messageView.schema->name != "foxglove.CompressedVideo") {
      continue;
    }
    const std::string& camera_name = it->second;

    foxglove::CompressedVideo msg;
    if (!msg.ParseFromArray(messageView.message.data,
                            static_cast<int>(messageView.message.dataSize))) {
      std::cerr << "Warning: Failed to parse CompressedVideo for " << camera_name << " frame "
                << out_streams[camera_name].frame_count << "\n";
      continue;
    }

    auto& stream = out_streams[camera_name];
    if (stream.frame_count == 0) {
      stream.format = msg.format();
      // ffmpeg infers the demuxer from the extension for raw elementary streams.
      const std::string ext = stream.format == "h265" ? ".hevc" : ".h264";
      const fs::path dir = dir_for(camera_name);
      stream.annexb_path = dir / (camera_name + ext);
      outputs[camera_name].open(stream.annexb_path, std::ios::binary);
      if (!outputs[camera_name]) {
        std::cerr << "Error: cannot open " << stream.annexb_path.string() << " for writing\n";
        return false;
      }
    } else if (msg.format() != stream.format) {
      // A camera that changed codec mid-episode cannot be remuxed as one stream.
      std::cerr << "Error: " << camera_name << " changed format from " << stream.format << " to "
                << msg.format() << " mid-episode\n";
      return false;
    }

    outputs[camera_name].write(msg.data().data(), static_cast<std::streamsize>(msg.data().size()));
    ++stream.frame_count;
  }

  for (auto& [camera_name, out] : outputs) {
    out.close();
    if (!out) {
      std::cerr << "Error: failed writing video stream for " << camera_name << "\n";
      return false;
    }
  }

  if (!out_streams.empty()) {
    std::cout << "  [ok] Extracted " << out_streams.size() << " compressed video stream(s)";
    for (const auto& [name, s] : out_streams) {
      std::cout << " " << name << "(" << s.format << ", " << s.frame_count << "f)";
    }
    std::cout << "\n";
  }
  return true;
}

nlohmann::ordered_json build_features(
  const AlignedEpisode& ep, const McapChannelMap& channels, bool native_schema) {
  nlohmann::ordered_json features;

  int joints_per_stream = ep.joints_per_stream > 0 ? ep.joints_per_stream : 7;

  // Joint names come from dataset_info when present, else positional fallbacks.
  auto get_joint_names = [&](const std::string& stream_id, int n) -> nlohmann::json {
    // Native lerobot_trossen naming: `<side>_joint_<i>.pos`, with the gripper
    // (last joint) named `<side>_left_carriage_joint.pos`.
    if (native_schema) {
      nlohmann::json names = nlohmann::json::array();
      const std::string side = stream_side(stream_id);
      const std::string prefix = side.empty() ? "" : side + "_";
      for (int i = 0; i < n; ++i) {
        names.push_back(i == n - 1 ? prefix + "left_carriage_joint.pos"
                                   : prefix + "joint_" + std::to_string(i) + ".pos");
      }
      return names;
    }
    if (!ep.mcap_dataset_info.empty() && ep.mcap_dataset_info.contains("streams") &&
        ep.mcap_dataset_info["streams"].contains(stream_id) &&
        ep.mcap_dataset_info["streams"][stream_id].contains("joint_names")) {
      return ep.mcap_dataset_info["streams"][stream_id]["joint_names"];
    }
    nlohmann::json names = nlohmann::json::array();
    std::string arm_name = stream_id;
    size_t underscore_pos = arm_name.find('_');
    if (underscore_pos != std::string::npos) {
      arm_name = arm_name.substr(underscore_pos + 1);
    }
    for (int i = 0; i < n; ++i) {
      names.push_back(arm_name + "_joint_" + std::to_string(i));
    }
    return names;
  };

  std::vector<std::string> base_vel_names = {"linear_vel", "angular_vel"};
  if (!ep.mcap_dataset_info.empty() && ep.mcap_dataset_info.contains("base_velocity_names")) {
    base_vel_names = ep.mcap_dataset_info["base_velocity_names"].get<std::vector<std::string>>();
  }

  // observation.state (followers)
  nlohmann::json obs_names = nlohmann::json::array();
  for (const auto& follower_stream : ep.follower_streams) {
    for (const auto& n : get_joint_names(follower_stream, joints_per_stream)) {
      obs_names.push_back(n);
    }
  }
  int obs_state_dim = static_cast<int>(ep.follower_streams.size()) * joints_per_stream;
  if (ep.has_slate_base) {
    for (const auto& n : base_vel_names) obs_names.push_back(n);
    obs_state_dim += static_cast<int>(base_vel_names.size());
  }
  features["observation.state"]["dtype"] = "float32";
  features["observation.state"]["shape"] = nlohmann::json::array({obs_state_dim});
  features["observation.state"]["names"] = obs_names;

  // action (leaders)
  nlohmann::json action_names = nlohmann::json::array();
  for (const auto& leader_stream : ep.leader_streams) {
    for (const auto& n : get_joint_names(leader_stream, joints_per_stream)) {
      action_names.push_back(n);
    }
  }
  int action_dim = static_cast<int>(ep.leader_streams.size()) * joints_per_stream;
  if (ep.has_slate_base) {
    for (const auto& n : base_vel_names) action_names.push_back(n);
    action_dim += static_cast<int>(base_vel_names.size());
  }
  features["action"]["dtype"] = "float32";
  features["action"]["shape"] = nlohmann::json::array({action_dim});
  features["action"]["names"] = action_names;

  // observation.images.<camera> video features
  for (const auto& [channel_id, camera_name] : channels.camera_channels) {
    // Feature key uses the native `cam_*` name; dataset_info is still keyed by the original name.
    const std::string out_name = native_schema ? native_camera_key(camera_name) : camera_name;
    const std::string obs_key = "observation.images." + out_name;
    const bool is_depth = native_schema && camera_is_depth(camera_name, ep.mcap_dataset_info);

    features[obs_key]["dtype"] = "video";
    features[obs_key]["names"] = nlohmann::json::array({"height", "width", "channels"});

    int h = 480, w = 640, ch = is_depth ? 1 : 3;
    int fps = 30;
    if (!ep.mcap_dataset_info.empty() && ep.mcap_dataset_info.contains("cameras") &&
        ep.mcap_dataset_info["cameras"].contains(camera_name)) {
      const auto& cam = ep.mcap_dataset_info["cameras"][camera_name];
      h = cam.value("height", h);
      w = cam.value("width", w);
      ch = cam.value("channels", ch);
      fps = cam.value("fps", fps);
      features[obs_key]["info"]["has_audio"] = cam.value("has_audio", false);
    } else {
      features[obs_key]["info"]["has_audio"] = false;
    }
    features[obs_key]["shape"] = nlohmann::json::array({h, w, ch});
    features[obs_key]["info"]["video.fps"] = fps;
    features[obs_key]["info"]["video.height"] = h;
    features[obs_key]["info"]["video.width"] = w;
    features[obs_key]["info"]["video.channels"] = ch;
    features[obs_key]["info"]["video.is_depth_map"] = is_depth;

    if (is_depth) {
      // Match lerobot 0.6.0 DepthEncoderConfig: HEVC Main 12 / gray12le, 12-bit log-quant.
      features[obs_key]["info"]["video.codec"] = "hevc";
      features[obs_key]["info"]["video.pix_fmt"] = "gray12le";
      features[obs_key]["info"]["video.depth_min"] = 0.01;   // metres, quantum 0
      features[obs_key]["info"]["video.depth_max"] = 10.0;   // metres, quantum DEPTH_QMAX
      features[obs_key]["info"]["video.shift"] = 3.5;        // metres, pre-log offset
      features[obs_key]["info"]["video.use_log"] = true;
      // lerobot reads the stored unit as info["depth_unit"] (no "video." prefix).
      features[obs_key]["info"]["depth_unit"] = "mm";  // raw mono16 depth is millimetres
    } else {
      // "libsvtav1", not "av1": lerobot validates video.codec against
      // VALID_VIDEO_CODECS, where the bare "av1" survives only through the
      // legacy VIDEO_CODECS_ALIASES map. Overridden per camera by the writer
      // when a recording's stream was remuxed rather than re-encoded.
      features[obs_key]["info"]["video.codec"] = "libsvtav1";
      features[obs_key]["info"]["video.pix_fmt"] = "yuv420p";
    }
  }

  return features;
}

}  // namespace trossen::convert
