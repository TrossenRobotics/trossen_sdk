/**
 * @file mcap_dataset_loader.cpp
 * @brief Implementation of the TrossenMCAP read + alignment path.
 *
 * This translation unit owns the single MCAP_IMPLEMENTATION definition for the SDK;
 * anything linking trossen_sdk must not define the macro.
 */

#define MCAP_IMPLEMENTATION
#include "trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <regex>
#include <string_view>

#include <opencv2/opencv.hpp>

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"

#include "JointState.pb.h"
#include "Odometry2D.pb.h"
#include "CompressedVideo.pb.h"
#include "RawImage.pb.h"

namespace trossen::io::backends {

namespace {

/// @brief mcap reader callback: log a recoverable parsing issue and keep reading.
void on_problem(const mcap::Status& problem) {
  std::cerr << "Warning: MCAP parsing issue: " << problem.message << "\n";
}

}  // namespace

bool load_aligned_episode(
  const std::string& mcap_file,
  int episode_index,
  AlignedEpisode& out,
  McapChannelMap& channels,
  const DatasetSignalOptions& signals,
  const AlignmentOptions& alignment)
{
  out = AlignedEpisode{};
  channels = McapChannelMap{};
  out.episode_index = episode_index;
  out.fps = static_cast<float>(alignment.fps);

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
  auto range = meta_indexes.equal_range(trossen_mcap_defs::kRecordingMetadataName);
  for (auto it = range.first; it != range.second; ++it) {
    mcap::Record raw_record;
    auto rs = mcap::McapReader::ReadRecord(*data_source, it->second.offset, &raw_record);
    if (!rs.ok()) continue;

    mcap::Metadata meta_record;
    rs = mcap::McapReader::ParseMetadata(raw_record, &meta_record);
    if (!rs.ok()) continue;

    auto info_it = meta_record.metadata.find(trossen_mcap_defs::kDatasetInfoKey);
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

  // Every topic seen, so a detection failure below can say what the recording did hold.
  std::vector<std::string> all_topics;

  std::cout << "  Available channels:\n";
  for (const auto& [channel_id, channel_ptr] : reader.channels()) {
    std::string topic = channel_ptr->topic;
    std::cout << "    - Topic: '" << topic << "'\n";
    all_topics.push_back(topic);

    size_t odom_pos = topic.find(trossen_mcap_defs::kOdometry2DTopicSuffix);
    if (odom_pos != std::string::npos) {
      std::string stream_id = topic.substr(0, odom_pos);
      if (!stream_id.empty() && stream_id[0] == '/') {
        stream_id = stream_id.substr(1);
      }
      channels.mobile_base_channel_id = channel_id;
      channels.has_mobile_base = true;
      std::cout << "    [ok] Found odometry stream for mobile robot: " << stream_id << "\n";
      continue;
    }

    size_t pos = topic.find(trossen_mcap_defs::kJointStateTopicSuffix);
    if (pos != std::string::npos) {
      std::string stream_id = topic.substr(0, pos);
      if (!stream_id.empty() && stream_id[0] == '/') {
        stream_id = stream_id.substr(1);
      }
      channels.joint_channels[channel_id] = stream_id;

      if (stream_id.find(trossen_mcap_defs::kLeaderStreamToken) != std::string::npos) {
        detected_leader_streams.push_back(stream_id);
        std::cout << "    [ok] Detected leader stream: " << stream_id << "\n";
      } else if (stream_id.find(trossen_mcap_defs::kFollowerStreamToken) != std::string::npos) {
        detected_follower_streams.push_back(stream_id);
        std::cout << "    [ok] Detected follower stream: " << stream_id << "\n";
      }
    }

    // Topic format: /cameras/<camera_name>/image, with the camera name as the capture
    // group. Built from the topic constants so the writer and reader cannot drift apart.
    {
      static const std::regex camera_topic_re(
        std::string("^") + trossen_mcap_defs::kCameraTopicPrefix + "(.+)" +
        trossen_mcap_defs::kImageTopicSuffix + "$");
      std::smatch m;
      if (std::regex_match(topic, m, camera_topic_re)) {
        channels.camera_channels[channel_id] = m[1].str();
      }
    }
  }

  // Naming the topics that were present separates an empty recording from one whose
  // topics simply do not follow the convention.
  auto report_topics = [&all_topics]() {
    if (all_topics.empty()) {
      std::cerr << "  The recording has no channels at all.\n";
      return;
    }
    std::cerr << "  Expected a topic ending in '" << trossen_mcap_defs::kJointStateTopicSuffix
              << "'. The recording holds:\n";
    for (const auto& topic : all_topics) std::cerr << "    - " << topic << "\n";
  };

  if (channels.joint_channels.empty()) {
    std::cerr << "Error: No joint state channels found in MCAP file\n";
    report_topics();
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
    // Fallback: single-robot mode, where every non-base stream is both leader and follower.
    // TODO(shantanuparab-tr): revisit this fallback. No supported robot records a stream
    // that is genuinely both the leader and the follower, so a recording reaching here is
    // more likely to be one whose stream ids miss the leader/follower tokens. Decide
    // whether to keep converting it or to reject it and report the naming mismatch.
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
      report_topics();
      return false;
    }
    out.leader_streams = all_streams;
    out.follower_streams = all_streams;
    std::cerr << "\n  Warning: no stream id contains '" << trossen_mcap_defs::kLeaderStreamToken
              << "' paired with one containing '" << trossen_mcap_defs::kFollowerStreamToken
              << "', so the leader/follower split cannot be read from the names. Found:\n    ";
    for (const auto& s : all_streams) std::cerr << s << " ";
    std::cerr << "\n  Treating all " << all_streams.size()
              << " stream(s) as both leader and follower.\n";
  }

  if (!channels.camera_channels.empty()) {
    std::cout << "  Found " << channels.camera_channels.size() << " camera channel(s)\n";
  }

  // ── Single pass over the message stream: joint states and odometry are parsed into
  //    per-stream buffers, and every camera frame's log time is recorded ──
  std::cout << "\nParsing recorded messages...\n";
  std::map<std::string, std::vector<data::JointStateRecord>> messages_by_stream;
  std::vector<data::Odometry2DRecord> mobile_base_messages;
  // Per camera, the log time of every frame in arrival order. The index into this vector is
  // the same source index extract_camera_images() counts up as it re-reads the file.
  std::map<std::string, std::vector<uint64_t>> camera_timestamps;

  size_t total_messages = 0;
  size_t total_images = 0;

  for (const auto& messageView : reader.readMessages(on_problem)) {
    if (channels.has_mobile_base && messageView.channel->id == channels.mobile_base_channel_id) {
      trossen_sdk::msg::Odometry2D odom_msg;
      if (!odom_msg.ParseFromArray(reinterpret_cast<const char*>(messageView.message.data),
                                   messageView.message.dataSize)) {
        std::cerr << "Warning: Failed to parse Odometry2D message\n";
        continue;
      }
      data::Odometry2DRecord rec;
      rec.ts.realtime = data::Timespec::from_ns(messageView.message.logTime);
      rec.seq = odom_msg.seq();
      rec.pose.x = odom_msg.pose().x();
      rec.pose.y = odom_msg.pose().y();
      rec.pose.theta = odom_msg.pose().theta();
      rec.twist.linear_x = odom_msg.twist().linear_x();
      rec.twist.linear_y = odom_msg.twist().linear_y();
      rec.twist.angular_z = odom_msg.twist().angular_z();
      mobile_base_messages.push_back(std::move(rec));
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

      data::JointStateRecord rec;
      rec.ts.realtime = data::Timespec::from_ns(messageView.message.logTime);
      rec.seq = js_msg.seq();
      rec.id = stream_id;
      rec.positions.assign(js_msg.positions().begin(), js_msg.positions().end());
      rec.velocities.assign(js_msg.velocities().begin(), js_msg.velocities().end());
      rec.efforts.assign(js_msg.efforts().begin(), js_msg.efforts().end());
      messages_by_stream[stream_id].push_back(std::move(rec));
      ++total_messages;
      continue;
    }

    auto camera_it = channels.camera_channels.find(messageView.channel->id);
    if (camera_it != channels.camera_channels.end()) {
      camera_timestamps[camera_it->second].push_back(messageView.message.logTime);
      ++total_images;
    }
  }

  std::cout << "  [ok] Parsed " << total_messages << " joint state messages\n";
  for (const auto& [stream_id, messages] : messages_by_stream) {
    std::cout << "    - " << stream_id << ": " << messages.size() << " messages\n";
  }
  if (channels.has_mobile_base) {
    std::cout << "    - mobile base: " << mobile_base_messages.size() << " messages (velocities)\n";
  }
  if (total_images > 0) {
    std::cout << "  [ok] Found " << total_images << " camera images\n";
    for (const auto& [camera_name, stamps] : camera_timestamps) {
      std::cout << "    - " << camera_name << ": " << stamps.size() << " frames\n";
    }
  }

  // ── Compute action/observation dimensions from the detected streams ──
  out.joints_per_stream = 0;
  for (const auto& [stream_id, messages] : messages_by_stream) {
    if (!messages.empty()) {
      out.joints_per_stream = static_cast<int>(messages[0].positions.size());
      break;
    }
  }
  out.has_mobile_base = channels.has_mobile_base;

  // Signal blocks per stream: positions, plus velocities and efforts when enabled. Only
  // observation.state carries the extra blocks; action stays positions-only.
  const int obs_blocks_per_stream =
    1 + (signals.joint_velocity ? 1 : 0) + (signals.joint_effort ? 1 : 0);

  // Base block: the planar velocity pair, plus the optional lateral velocity and pose.
  const int base_block_width =
    2 + (signals.base_lateral_velocity ? 1 : 0) + (signals.base_pose ? 3 : 0);

  // Width of every action row: one positions block per leader stream.
  out.action_dim = static_cast<int>(out.leader_streams.size()) * out.joints_per_stream;
  // Width of every observation row: each follower stream contributes obs_blocks_per_stream
  // blocks of joints_per_stream values.
  out.obs_dim =
    static_cast<int>(out.follower_streams.size()) * out.joints_per_stream * obs_blocks_per_stream;
  if (out.has_mobile_base) {
    out.action_dim += base_block_width;
    out.obs_dim += base_block_width;
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

  const size_t max_rows = reference_messages.size();

  // Record the cameras present. Built before the row loop so each row can store the frame
  // it matched; frame_count is filled in by extract_camera_images().
  for (const auto& [channel_id, camera_name] : channels.camera_channels) {
    if (camera_timestamps[camera_name].empty()) {
      std::cerr << "Error: camera '" << camera_name << "' has a channel but no frames\n";
      return false;
    }
    CameraInfo cam;
    // The recording's own camera name keys the extraction dirs, the dataset_info lookups
    // and the LeRobot observation column alike.
    cam.name = camera_name;
    cam.obs_key = "observation.images." + camera_name;
    cam.row_source_index.reserve(max_rows);
    out.cameras.push_back(std::move(cam));
  }

  // TODO(shantanuparab-tr): implement the nearest-timestamp row alignment.

  return true;
}

bool extract_camera_images(
  const std::string& mcap_file,
  const McapChannelMap& channels,
  const AlignedEpisode& episode,
  const std::function<std::filesystem::path(const std::string& camera_name)>& dir_for,
  std::map<std::string, size_t>& out_counts,
  bool native_schema)
{
  // TODO(shantanuparab-tr): implement the per-row camera frame decode and write.
  return false;
}

bool extract_camera_video(
  const std::string& mcap_file,
  const McapChannelMap& channels,
  const std::function<std::filesystem::path(const std::string& camera_name)>& dir_for,
  std::map<std::string, CameraVideoStream>& out_streams)
{
  // TODO(shantanuparab-tr): implement the Annex B elementary stream extraction.
  return false;
}

void clamp_episode_to_video_frame_counts(
  AlignedEpisode& ep, const std::map<std::string, CameraVideoStream>& video_streams) {
  // TODO(shantanuparab-tr): implement the trim to the shortest video-mode camera.
}

}  // namespace trossen::io::backends
