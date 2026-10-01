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
#include <atomic>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>

#include <opencv2/opencv.hpp>

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"

#include "JointState.pb.h"
#include "Odometry2D.pb.h"
#include "CompressedVideo.pb.h"
#include "FrameMeta.pb.h"
#include "RawImage.pb.h"

namespace trossen::io::backends {

namespace {

/// @brief mcap reader callback: log a recoverable parsing issue and keep reading.
void on_problem(const mcap::Status& problem) {
  std::cerr << "Warning: MCAP parsing issue: " << problem.message << "\n";
}

/// @brief Warn, once per process, that Rivet recordings have no LeRobot support yet.
///
/// The episode still converts, but only the planar linear and angular base velocity
/// reach the dataset (no lateral velocity, no lift), and no LeRobot robot consumes the
/// result. Printed once because a folder run loads every episode, some on worker threads.
void warn_rivet_unsupported() {
  static std::atomic<bool> warned{false};
  if (warned.exchange(true)) return;
  std::cerr << "\nWARNING: LeRobot conversion does not support Rivet recordings yet. The "
            << "dataset is written for inspection only: the base keeps its linear and "
            << "angular velocity but drops the lateral velocity and the lift, and no LeRobot "
            << "robot can train on or replay it.\n\n";
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
      // The Rivet example records its base as `trossen_base` on robot `trossen_rivet`.
      if (stream_id == "trossen_base" || out.robot_name == "trossen_rivet") {
        warn_rivet_unsupported();
      }
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
      static const std::regex camera_meta_topic_re(
        std::string("^") + trossen_mcap_defs::kCameraTopicPrefix + "(.+)" +
        trossen_mcap_defs::kCameraMetaTopicSuffix + "$");
      std::smatch m;
      if (std::regex_match(topic, m, camera_topic_re)) {
        channels.camera_channels[channel_id] = m[1].str();
      } else if (std::regex_match(topic, m, camera_meta_topic_re) && channel_ptr->schemaId != 0 &&
                 reader.schema(channel_ptr->schemaId) &&
                 reader.schema(channel_ptr->schemaId)->name == "trossen_sdk.msg.FrameMeta") {
        channels.camera_meta_channels[channel_id] = m[1].str();
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

  // A leader device need not carry the leader token (the Glide records as glide_left /
  // glide_right), so when only followers are named, every other arm stream leads.
  if (detected_leader_streams.empty() && !detected_follower_streams.empty()) {
    for (const auto& [channel_id, stream_id] : channels.joint_channels) {
      if (stream_id != "slate_base" &&
          std::find(detected_follower_streams.begin(), detected_follower_streams.end(),
                    stream_id) == detected_follower_streams.end()) {
        detected_leader_streams.push_back(stream_id);
        std::cout << "    [ok] Inferred leader stream: " << stream_id << "\n";
      }
    }
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
  // Per camera, what the camera itself recorded for each frame, indexed by the frame's
  // position in the stream (FrameMeta.frame_index, the same index as camera_timestamps).
  struct CaptureRecord {
    bool has_device{false};
    uint64_t device_ns{0};
    std::optional<uint64_t> frame_number;
  };
  std::map<std::string, std::vector<CaptureRecord>> camera_capture;

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
      continue;
    }

    auto meta_it = channels.camera_meta_channels.find(messageView.channel->id);
    if (meta_it != channels.camera_meta_channels.end()) {
      trossen_sdk::msg::FrameMeta meta;
      if (!meta.ParseFromArray(reinterpret_cast<const char*>(messageView.message.data),
                               static_cast<int>(messageView.message.dataSize))) {
        continue;
      }
      auto& records = camera_capture[meta_it->second];
      const size_t index = meta.frame_index();
      if (index >= records.size()) records.resize(index + 1);
      CaptureRecord& record = records[index];
      record.has_device = meta.ts().device_clock() != trossen_sdk::DEVICE_CLOCK_NONE;
      if (record.has_device) {
        record.device_ns = static_cast<uint64_t>(meta.ts().device().seconds()) * data::S_TO_NS +
                           static_cast<uint64_t>(meta.ts().device().nanos());
      }
      if (meta.has_device_frame_number()) record.frame_number = meta.device_frame_number();
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

  // Records carry a dual (sec, nsec) Timestamp; the MCAP log time was written from the
  // realtime half, so that is the key every comparison below uses.
  auto log_ns_of = [](const data::RecordBase& rec) { return rec.ts.realtime.to_ns(); };

  // Every stream must be in log-time order: the window reads each stream's first and last
  // sample as its earliest and latest, and rows are matched with cursors that only move
  // forward. The realtime clock can step backwards (an NTP correction), which would break
  // both without any error, so an out-of-order stream rejects the episode.
  auto check_time_order = [](const std::string& name, size_t count,
                             const auto& stamp_at) -> bool {
    for (size_t i = 1; i < count; ++i) {
      if (stamp_at(i) < stamp_at(i - 1)) {
        std::cerr << "Error: " << name << " timestamps go backwards at message " << i << " ("
                  << (stamp_at(i - 1) - stamp_at(i)) << " ns earlier than message " << (i - 1)
                  << "). The recording clock stepped, so rows cannot be matched.\n";
        return false;
      }
    }
    return true;
  };
  for (const auto& [stream_id, messages] : messages_by_stream) {
    if (!check_time_order(stream_id, messages.size(),
                          [&](size_t i) { return log_ns_of(messages[i]); })) {
      return false;
    }
  }
  if (!check_time_order("mobile base", mobile_base_messages.size(),
                        [&](size_t i) { return log_ns_of(mobile_base_messages[i]); })) {
    return false;
  }
  for (const auto& [camera_name, stamps] : camera_timestamps) {
    const auto stamp_at = [&stamps](size_t i) { return stamps[i]; };
    if (!check_time_order(camera_name, stamps.size(), stamp_at)) {
      return false;
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

  // ── Require joint data: a follower stream, or failing that any stream (single-robot) ──
  bool have_follower_data = false;
  for (const auto& stream : out.follower_streams) {
    auto it = messages_by_stream.find(stream);
    if (it != messages_by_stream.end() && !it->second.empty()) {
      have_follower_data = true;
      break;
    }
  }
  if (!have_follower_data) {
    bool have_any_data = false;
    for (const auto& [stream_id, msgs] : messages_by_stream) {
      if (!msgs.empty()) {
        have_any_data = true;
        std::cout << "  Note: Using single-robot mode with stream: " << stream_id << "\n";
        out.leader_streams = {stream_id};
        out.follower_streams = {stream_id};
        break;
      }
    }
    if (!have_any_data) {
      std::cerr << "Error: No joint state streams found in MCAP file\n";
      return false;
    }
  }

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
    out.cameras.push_back(std::move(cam));
  }

  // Rows sit on a uniform grid at the dataset rate, spanning the window where every
  // stream has data. A LeRobot episode plays row k back at k/fps, so uniform spacing is
  // what makes that stored timestamp describe the row.
  const uint64_t row_period_ns =
    static_cast<uint64_t>(static_cast<double>(data::S_TO_NS) / alignment.fps);

  // The streams that set each edge are kept so a window that closes can be reported.
  uint64_t window_start = 0;
  uint64_t window_end = std::numeric_limits<uint64_t>::max();
  std::string last_to_start;
  std::string first_to_end;
  auto narrow_window = [&](const std::string& name, uint64_t first, uint64_t last) {
    if (first >= window_start) {
      window_start = first;
      last_to_start = name;
    }
    if (last <= window_end) {
      window_end = last;
      first_to_end = name;
    }
  };

  for (const auto& stream_id : out.leader_streams) {
    const auto it = messages_by_stream.find(stream_id);
    if (it == messages_by_stream.end() || it->second.empty()) continue;
    narrow_window(stream_id, log_ns_of(it->second.front()), log_ns_of(it->second.back()));
  }
  for (const auto& stream_id : out.follower_streams) {
    const auto it = messages_by_stream.find(stream_id);
    if (it == messages_by_stream.end() || it->second.empty()) continue;
    narrow_window(stream_id, log_ns_of(it->second.front()), log_ns_of(it->second.back()));
  }
  for (const auto& cam : out.cameras) {
    const auto& stamps = camera_timestamps[cam.name];
    if (stamps.empty()) continue;
    narrow_window(cam.name, stamps.front(), stamps.back());
  }

  // At least one joint stream has data, so the window always has a finite end. A window
  // that closed means one stream stopped before another started, and no row can hold a
  // sample from both.
  if (window_end <= window_start) {
    std::cerr << "Error: no stretch of time has data from every stream. " << first_to_end
              << " ends " << static_cast<double>(window_start - window_end) / 1e6
              << " ms before " << last_to_start << " starts.\n";
    return false;
  }

  const uint64_t span_ns = window_end - window_start;
  const size_t row_count = static_cast<size_t>(span_ns / row_period_ns) + 1;
  std::vector<uint64_t> row_times;
  row_times.reserve(row_count);
  for (size_t k = 0; k < row_count; ++k) {
    row_times.push_back(window_start + k * row_period_ns);
  }
  std::cout << "  Rows run on a " << alignment.fps << " Hz grid over "
            << static_cast<double>(span_ns) / static_cast<double>(data::S_TO_NS) << " s ("
            << row_times.size() << " rows)\n";

  const size_t max_rows = row_times.size();
  for (auto& cam : out.cameras) cam.row_source_index.reserve(max_rows);

  // ── Align: for each row time, snap every stream to its nearest sample ──
  std::map<std::string, size_t> stream_indices;
  for (const auto& [stream_id, _] : messages_by_stream) {
    stream_indices[stream_id] = 0;
  }

  const double frame_duration_s = 1.0 / alignment.fps;
  size_t mobile_base_idx = 0;
  int64_t frame_index = 0;
  size_t rows_skipped = 0;
  size_t rows_skipped_no_frame = 0;

  // A row dropped between two kept rows is a gap. Rows are stamped by position, so every
  // row after a gap plays one period early per dropped row. Rows dropped before the first
  // kept row or after the last one only shorten the episode and are not gaps.
  // Each gap keeps the stream that had no sample in its first dropped row and, when that
  // stream is a camera, the camera's frames either side of the gap.
  struct Gap {
    size_t first_row{0};
    size_t length{0};
    std::string missing_stream;
    std::optional<size_t> camera_index;
    size_t frame_before{0};
    size_t frame_after{0};
  };
  std::vector<Gap> gaps;
  Gap pending_gap{};
  auto note_skipped_row = [&](size_t row, const std::string& missing,
                              std::optional<size_t> camera_index) {
    if (out.frames.empty()) return;
    if (pending_gap.length == 0) {
      pending_gap = Gap{row, 0, missing, camera_index, 0, 0};
      if (camera_index) {
        pending_gap.frame_before = out.cameras[*camera_index].row_source_index.back();
      }
    }
    ++pending_gap.length;
  };

  // Per-camera search cursor. Rows are visited in increasing time, so each cursor only
  // ever moves forward.
  std::map<std::string, size_t> camera_cursors;
  for (const auto& [camera_name, stamps] : camera_timestamps) camera_cursors[camera_name] = 0;

  // Index of the entry nearest `target` in a time-ordered sequence of `count` entries, or
  // npos when the nearest is further away than the tolerance. `stamp_at(i)` returns entry
  // i's time. Both neighbors are compared: snapping only to the last entry at or before
  // the target biases every row backwards by up to a full sample period, and a bias does
  // not average out the way jitter does. Shared by joint, mobile base and camera streams
  // so they all pair rows the same way.
  auto nearest_index = [&alignment](size_t count, const auto& stamp_at, uint64_t target,
                                    size_t& cursor) -> size_t {
    if (cursor >= count) return std::numeric_limits<size_t>::max();
    // Scan forward to the last entry whose time is at or before the target.
    while (cursor + 1 < count && stamp_at(cursor + 1) <= target) ++cursor;
    auto distance = [target](uint64_t ts) {
      return target > ts ? target - ts : ts - target;
    };
    // Pick whichever is closer to the target: that entry, or the next one past it.
    size_t best = cursor;
    if (cursor + 1 < count && distance(stamp_at(cursor + 1)) < distance(stamp_at(cursor))) {
      best = cursor + 1;
    }
    // Accept the closer entry only if it is within the tolerance.
    return distance(stamp_at(best)) > alignment.tolerance_ns ? std::numeric_limits<size_t>::max()
                                                             : best;
  };

  // Frame nearest `target`, or npos when the nearest is outside the tolerance.
  auto nearest_frame = [&](const std::vector<uint64_t>& stamps, uint64_t target,
                           size_t& cursor) -> size_t {
    return nearest_index(
      stamps.size(), [&](size_t i) { return stamps[i]; }, target, cursor);
  };

  // Joint sample nearest `target_ts`, or nullptr when the nearest is outside the tolerance.
  auto find_closest_message = [&](const std::string& stream_id, uint64_t target_ts,
                                  size_t& idx) -> const data::JointStateRecord* {
    auto it = messages_by_stream.find(stream_id);
    if (it == messages_by_stream.end()) return nullptr;
    const auto& messages = it->second;
    const size_t best = nearest_index(
      messages.size(), [&](size_t i) { return log_ns_of(messages[i]); }, target_ts, idx);
    return best == std::numeric_limits<size_t>::max() ? nullptr : &messages[best];
  };

  // Appends one stream's enabled signal blocks in the order build_features() names them:
  // positions, then velocities, then efforts. A stream that recorded fewer values than it
  // has joints is zero-filled so every row keeps the same width.
  auto append_joint_signals = [&](std::vector<double>& dst, const data::JointStateRecord& sample,
                                  bool with_velocity, bool with_effort) {
    const size_t n = sample.positions.size();
    // Each block contributes exactly n values: a short block is zero-filled and a long one
    // is truncated, so every row matches the width build_features() declares.
    auto append_block = [&](const std::vector<float>& src) {
      const size_t take = std::min(n, src.size());
      dst.insert(dst.end(), src.begin(), src.begin() + static_cast<std::ptrdiff_t>(take));
      dst.resize(dst.size() + (n - take), 0.0);
    };
    append_block(sample.positions);
    if (with_velocity) append_block(sample.velocities);
    if (with_effort) append_block(sample.efforts);
  };

  out.frames.reserve(max_rows);
  for (size_t ref_idx = 0; ref_idx < max_rows; ++ref_idx) {
    const uint64_t timestamp_ns = row_times[ref_idx];

    // First stream without a sample within tolerance, if the row is dropped.
    std::string missing_stream;

    std::vector<double> actions;
    bool have_all_leaders = true;
    for (const auto& leader_stream : out.leader_streams) {
      const auto* sample = find_closest_message(leader_stream, timestamp_ns,
                                                stream_indices[leader_stream]);
      if (sample) {
        actions.insert(actions.end(), sample->positions.begin(), sample->positions.end());
      } else {
        have_all_leaders = false;
        missing_stream = leader_stream;
        break;
      }
    }

    std::vector<double> observations;
    bool have_all_followers = true;
    for (const auto& follower_stream : out.follower_streams) {
      const auto* sample = find_closest_message(follower_stream, timestamp_ns,
                                                stream_indices[follower_stream]);
      if (sample) {
        append_joint_signals(observations, *sample, signals.joint_velocity,
                             signals.joint_effort);
      } else {
        have_all_followers = false;
        if (missing_stream.empty()) missing_stream = follower_stream;
        break;
      }
    }

    std::vector<double> base_values;
    if (channels.has_mobile_base) {
      // A base channel with no messages yields npos here, so the row is zero-filled below.
      const size_t base_idx = nearest_index(
        mobile_base_messages.size(),
        [&](size_t i) { return log_ns_of(mobile_base_messages[i]); }, timestamp_ns,
        mobile_base_idx);
      if (base_idx != std::numeric_limits<size_t>::max()) {
        const auto& odom = mobile_base_messages[base_idx];
        base_values.push_back(odom.twist.linear_x);
        base_values.push_back(odom.twist.angular_z);
        if (signals.base_lateral_velocity) base_values.push_back(odom.twist.linear_y);
        if (signals.base_pose) {
          base_values.push_back(odom.pose.x);
          base_values.push_back(odom.pose.y);
          base_values.push_back(odom.pose.theta);
        }
      }
      if (base_values.empty()) {
        base_values.assign(base_block_width, 0.0);
      }
    }

    if (!have_all_leaders || !have_all_followers) {
      ++rows_skipped;
      note_skipped_row(ref_idx, missing_stream, std::nullopt);
      continue;
    }

    // Every camera must offer a frame within tolerance, else the row would pair a joint
    // state with a stale image. Selections are staged and only committed once the whole
    // row is known good, so a late miss can't leave the cameras half-filled.
    std::vector<size_t> staged_camera_frames;
    staged_camera_frames.reserve(out.cameras.size());
    for (const auto& cam : out.cameras) {
      const size_t frame_idx = nearest_frame(camera_timestamps[cam.name], timestamp_ns,
                                             camera_cursors[cam.name]);
      if (frame_idx == std::numeric_limits<size_t>::max()) {
        missing_stream = cam.name;
        break;
      }
      staged_camera_frames.push_back(frame_idx);
    }
    if (staged_camera_frames.size() != out.cameras.size()) {
      ++rows_skipped_no_frame;
      // The first camera without a frame is the one the staging loop stopped at.
      note_skipped_row(ref_idx, missing_stream, staged_camera_frames.size());
      continue;
    }
    for (size_t c = 0; c < out.cameras.size(); ++c) {
      out.cameras[c].row_source_index.push_back(staged_camera_frames[c]);
    }

    if (channels.has_mobile_base) {
      actions.insert(actions.end(), base_values.begin(), base_values.end());
      observations.insert(observations.end(), base_values.begin(), base_values.end());
    }

    if (pending_gap.length > 0) {
      if (pending_gap.camera_index) {
        pending_gap.frame_after = staged_camera_frames[*pending_gap.camera_index];
      }
      gaps.push_back(pending_gap);
      pending_gap = Gap{};
    }

    AlignedFrame frame;
    frame.timestamp_s = static_cast<float>(static_cast<double>(frame_index) * frame_duration_s);
    frame.row_time_ns = timestamp_ns;
    frame.action = std::move(actions);
    frame.observation = std::move(observations);
    out.frames.push_back(std::move(frame));
    ++frame_index;
  }

  std::cout << "  [ok] Aligned " << out.frames.size() << " frames";
  if (rows_skipped > 0) std::cout << " (skipped " << rows_skipped << " misaligned)";
  if (rows_skipped_no_frame > 0) {
    std::cout << " (skipped " << rows_skipped_no_frame << " with no camera frame in tolerance)";
  }
  std::cout << "\n";

  if (!gaps.empty()) {
    auto gap_ms = [row_period_ns](size_t rows) {
      return static_cast<double>(rows * row_period_ns) / 1e6;
    };
    size_t gap_rows = 0;
    size_t longest_gap = 0;
    for (const auto& gap : gaps) {
      gap_rows += gap.length;
      longest_gap = std::max(longest_gap, gap.length);
    }
    const double gap_fraction =
      static_cast<double>(gap_rows) / static_cast<double>(row_times.size());
    const bool gap_too_long = gap_ms(longest_gap) > alignment.max_single_gap_ms;
    const bool too_many_gaps = gap_fraction > alignment.max_gap_fraction;

    const std::ios::fmtflags saved_flags = std::cerr.flags();
    const std::streamsize saved_precision = std::cerr.precision();
    std::cerr << (gap_too_long || too_many_gaps ? "Error: " : "Warning: ") << gap_rows
              << " row(s) dropped mid-episode (" << std::fixed << std::setprecision(2)
              << gap_fraction * 100.0 << "% of the grid), so every row after each gap "
              << "plays early:\n";
    // Host arrival alone cannot tell a camera that stopped capturing from one whose frames
    // were held up in USB or driver buffers and delivered together. The camera's own frame
    // counter, or failing that its device clock, can: a delayed camera still shows one
    // frame per period across the gap.
    std::map<std::string, double> device_period_ns;
    auto period_of = [&](const std::string& camera) -> double {
      auto cached = device_period_ns.find(camera);
      if (cached != device_period_ns.end()) return cached->second;
      std::vector<double> intervals;
      const auto& records = camera_capture[camera];
      for (size_t i = 1; i < records.size(); ++i) {
        if (records[i].has_device && records[i - 1].has_device &&
            records[i].device_ns > records[i - 1].device_ns) {
          intervals.push_back(static_cast<double>(records[i].device_ns - records[i - 1].device_ns));
        }
      }
      double period = 0.0;
      if (!intervals.empty()) {
        std::nth_element(intervals.begin(), intervals.begin() + intervals.size() / 2,
                         intervals.end());
        period = intervals[intervals.size() / 2];
      }
      device_period_ns[camera] = period;
      return period;
    };
    // Frames the camera lost across a gap, or nullopt when it recorded nothing to tell.
    auto frames_lost_across = [&](const Gap& gap) -> std::optional<uint64_t> {
      const auto& records = camera_capture[gap.missing_stream];
      if (gap.frame_after >= records.size() || gap.frame_after <= gap.frame_before) {
        return std::nullopt;
      }
      bool counters = true;
      bool clocks = true;
      for (size_t i = gap.frame_before; i <= gap.frame_after; ++i) {
        counters = counters && records[i].frame_number.has_value();
        clocks = clocks && records[i].has_device;
      }
      uint64_t lost = 0;
      if (counters) {
        for (size_t i = gap.frame_before + 1; i <= gap.frame_after; ++i) {
          const uint64_t step = *records[i].frame_number - *records[i - 1].frame_number;
          if (step > 1) lost += step - 1;
        }
        return lost;
      }
      const double period = period_of(gap.missing_stream);
      if (!clocks || period <= 0.0) return std::nullopt;
      for (size_t i = gap.frame_before + 1; i <= gap.frame_after; ++i) {
        if (records[i].device_ns <= records[i - 1].device_ns) return std::nullopt;
        const double steps =
          std::round(static_cast<double>(records[i].device_ns - records[i - 1].device_ns) / period);
        if (steps > 1.0) lost += static_cast<uint64_t>(steps) - 1;
      }
      return lost;
    };

    std::set<std::string> stalled_cameras;
    std::set<std::string> delayed_cameras;
    for (const auto& gap : gaps) {
      std::cerr << "    - " << gap.length << " row(s) (" << gap_ms(gap.length) << " ms) at "
                << static_cast<double>(row_times[gap.first_row] - row_times.front()) /
                     static_cast<double>(data::S_TO_NS)
                << " s, " << gap.missing_stream << " had no sample";
      if (gap.camera_index) {
        const std::optional<uint64_t> lost = frames_lost_across(gap);
        if (!lost) {
          std::cerr << "; no capture record to tell a stall from a delivery delay";
          stalled_cameras.insert(gap.missing_stream);
        } else if (*lost > 0) {
          std::cerr << "; the camera lost " << *lost << " frame(s)";
          stalled_cameras.insert(gap.missing_stream);
        } else {
          std::cerr << "; the camera captured every frame, delivery was delayed";
          delayed_cameras.insert(gap.missing_stream);
        }
      }
      std::cerr << "\n";
    }
    for (const auto& camera : stalled_cameras) delayed_cameras.erase(camera);

    // A delivery delay is not a camera fault: the frames exist, and only matching rows on
    // host arrival time drops them.
    if (!delayed_cameras.empty()) {
      std::cerr << "  Delivery delay, not a stall (";
      for (auto it = delayed_cameras.begin(); it != delayed_cameras.end(); ++it) {
        std::cerr << (it == delayed_cameras.begin() ? "" : ", ") << *it;
      }
      std::cerr << "): the camera captured every frame, but they reached the host late, so "
                << "rows matched on arrival time were dropped.\n";
    }

    // A camera stall is a recording fault. Dropping its rows drops them for every camera,
    // so each stream-copied video that kept recording lags its joints from the gap on, and
    // no conversion step can restore the frames the stalled camera never captured.
    if (!stalled_cameras.empty()) {
      std::cerr << "  BAD EPISODE: camera stall during recording (";
      for (auto it = stalled_cameras.begin(); it != stalled_cameras.end(); ++it) {
        std::cerr << (it == stalled_cameras.begin() ? "" : ", ") << *it;
      }
      std::cerr << "). Rows were dropped for every camera, so the video of any camera that "
                << "kept recording lags its joints after each gap. Conversion cannot repair "
                << "this; discard or re-record the episode.\n";
    }
    if (gap_too_long) {
      std::cerr << "  The longest gap, " << gap_ms(longest_gap) << " ms, is over the "
                << alignment.max_single_gap_ms << " ms limit; rejecting the episode.\n";
    }
    if (too_many_gaps) {
      std::cerr << "  Dropped rows are over the " << alignment.max_gap_fraction * 100.0
                << "% limit; rejecting the episode.\n";
    }
    std::cerr.flags(saved_flags);
    std::cerr.precision(saved_precision);
    if (gap_too_long || too_many_gaps) return false;
  }

  // How far each camera ends up from its rows: a large or growing offset means the camera
  // clock is drifting away from the row grid and is worth investigating.
  for (const auto& cam : out.cameras) {
    if (cam.row_source_index.empty()) continue;
    const auto& stamps = camera_timestamps[cam.name];
    double sum_abs_ms = 0.0;
    double worst_ms = 0.0;
    for (size_t row = 0; row < out.frames.size(); ++row) {
      const int64_t delta = static_cast<int64_t>(stamps[cam.row_source_index[row]]) -
                            static_cast<int64_t>(out.frames[row].row_time_ns);
      const double delta_ms = static_cast<double>(delta) / 1e6;
      sum_abs_ms += std::abs(delta_ms);
      worst_ms = std::max(worst_ms, std::abs(delta_ms));
    }
    std::cout << "    - " << cam.name << ": " << stamps.size() << " frames, pairing offset mean "
              << std::fixed << std::setprecision(1)
              << (sum_abs_ms / static_cast<double>(out.frames.size())) << " ms, worst " << worst_ms
              << " ms\n";
  }
  std::cout.unsetf(std::ios::floatfield);

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
  namespace fs = std::filesystem;
  out_counts.clear();
  if (channels.camera_channels.empty()) {
    return true;
  }

  // Invert the per-row selection: source frame index → the rows that matched it. A frame
  // matched by two consecutive rows is decoded once and written under both row numbers.
  std::map<std::string, std::map<size_t, std::vector<size_t>>> rows_by_source;
  std::map<std::string, size_t> expected_counts;
  for (const auto& cam : episode.cameras) {
    auto& mapping = rows_by_source[cam.name];
    for (size_t row = 0; row < cam.row_source_index.size(); ++row) {
      mapping[cam.row_source_index[row]].push_back(row);
    }
    expected_counts[cam.name] = cam.row_source_index.size();
  }

  std::map<std::string, fs::path> camera_dirs;
  for (const auto& [channel_id, camera_name] : channels.camera_channels) {
    camera_dirs[camera_name] = dir_for(camera_name);
    out_counts[camera_name] = 0;
  }

  // Arrival counter per camera, mirroring the indices load_aligned_episode() assigned.
  std::map<std::string, size_t> source_indices;

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
    const size_t source_idx = source_indices[camera_name]++;

    // Only frames some row matched are decoded; the rest are read past.
    const auto& mapping = rows_by_source[camera_name];
    auto rows_it = mapping.find(source_idx);
    if (rows_it == mapping.end()) continue;
    const std::vector<size_t>& target_rows = rows_it->second;

    foxglove::RawImage raw_image;
    if (!raw_image.ParseFromArray(messageView.message.data,
                                  static_cast<int>(messageView.message.dataSize))) {
      std::cerr << "Error: Failed to parse RawImage message for " << camera_name << " source frame "
                << source_idx << " (needed by row " << target_rows.front() << ")\n";
      return false;
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
      std::cerr << "Error: Unsupported encoding '" << raw_image.encoding() << "' for "
                << camera_name << " source frame " << source_idx << "\n";
      return false;
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
      std::cerr << "Error: Empty image for " << camera_name << " source frame " << source_idx
                << "\n";
      return false;
    }

    // Native schema preserves 16-bit depth losslessly as PNG (JPEG is 8-bit and would
    // destroy the depth). RGB/8-bit stays JPEG. The writer picks its encode path by the
    // frame extension it finds (.png → gray12le HEVC depth, .jpg → av1 RGB).
    const bool depth_png = native_schema && cv_type == CV_16UC1;
    std::vector<int> compression_params =
      depth_png ? std::vector<int>{cv::IMWRITE_PNG_COMPRESSION, 1}
                : std::vector<int>{cv::IMWRITE_JPEG_QUALITY, 95};

    // Frames are numbered by row, not by arrival, so the encoded video lines up with the
    // parquet rows one-for-one.
    for (size_t row : target_rows) {
      char namebuf[32];
      std::snprintf(namebuf, sizeof(namebuf), depth_png ? "image_%06zu.png" : "image_%06zu.jpg",
                    row);
      fs::path image_path = camera_dirs[camera_name] / namebuf;
      if (!cv::imwrite(image_path.string(), image_copy, compression_params)) {
        std::cerr << "Error: Failed to save image: " << image_path.string() << "\n";
        return false;
      }
      ++images_saved;
      out_counts[camera_name]++;
    }
  }

  // A short camera means the video would silently run out of frames before the rows do.
  for (const auto& [camera_name, expected] : expected_counts) {
    if (out_counts[camera_name] != expected) {
      std::cerr << "Error: " << camera_name << " wrote " << out_counts[camera_name]
                << " frames but " << expected << " rows need one\n";
      return false;
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

size_t video_start_offset(const CameraInfo& cam) {
  return cam.row_source_index.empty() ? 0 : cam.row_source_index.front();
}

void report_unaligned_video_streams(
  const AlignedEpisode& ep, const std::map<std::string, CameraVideoStream>& video_streams,
  bool start_offset_applied) {
  // A video stream is copied out packet by packet in recording order, starting at the
  // camera's first frame, and muxed at a constant rate. The copy cannot start mid-stream,
  // because every frame after a keyframe is stored as a change from the one before it.
  // Row n therefore plays frame n + offset only when the writer moves the video's start
  // to the frame matched to row 0; otherwise row n plays frame n. A change in the step is
  // a dropped or duplicated frame, which shifts every later row's image and nothing
  // downstream can tell.
  for (const auto& cam : ep.cameras) {
    auto it = video_streams.find(cam.name);
    if (it == video_streams.end() || it->second.frame_count == 0) continue;
    if (cam.row_source_index.empty()) continue;

    const size_t offset = video_start_offset(cam);
    if (offset > 0 && !start_offset_applied) {
      std::ostringstream lag_ms;
      lag_ms << std::fixed << std::setprecision(1)
             << static_cast<double>(offset) * 1000.0 / static_cast<double>(ep.fps);
      std::cerr << "\n  WARNING: " << cam.name << " video is offset by " << offset
                << " frame(s) (" << lag_ms.str() << " ms). Row 0 was matched to frame "
                << offset << ", but the copied video starts at frame 0, so every image "
                << "lags its joint state by " << lag_ms.str() << " ms. This format "
                << "cannot start a copied video mid-stream; convert with "
                << "trossen_mcap_to_lerobot_v3 for offset-correct video.\n\n";
    }

    size_t first_mismatch = cam.row_source_index.size();
    for (size_t row = 1; row < cam.row_source_index.size(); ++row) {
      if (cam.row_source_index[row] != offset + row) {
        first_mismatch = row;
        break;
      }
    }
    const size_t covered = ep.frames.size() + offset;
    const size_t extra =
      it->second.frame_count > covered ? it->second.frame_count - covered : 0;
    if (first_mismatch == cam.row_source_index.size() && extra == 0) continue;

    std::cerr << "Warning: " << cam.name << " video is not row aligned. ";
    if (first_mismatch < cam.row_source_index.size()) {
      std::cerr << "row " << first_mismatch << " wanted frame "
                << cam.row_source_index[first_mismatch] << ", not "
                << (offset + first_mismatch) << ". ";
    }
    if (extra > 0) {
      std::cerr << extra << " frame(s) past the last row. ";
    }
    std::cerr << "Copying the stream as recorded pairs row n with frame n + "
              << (start_offset_applied ? offset : 0)
              << ", so the images drift from the joint data.\n";
  }
}

void clamp_episode_to_video_frame_counts(
  AlignedEpisode& ep, const std::map<std::string, CameraVideoStream>& video_streams,
  bool start_offset_applied) {
  size_t playable_frames = std::numeric_limits<size_t>::max();
  for (const auto& cam : ep.cameras) {
    auto it = video_streams.find(cam.name);
    if (it == video_streams.end() || it->second.frame_count == 0) continue;
    // Frames before the one matched to row 0 are never played once the start is moved.
    const size_t skipped = start_offset_applied ? video_start_offset(cam) : 0;
    const size_t frames = it->second.frame_count;
    playable_frames = std::min(playable_frames, frames > skipped ? frames - skipped : 0);
  }
  if (playable_frames < ep.frames.size()) {
    ep.frames.resize(playable_frames);
    for (auto& cam : ep.cameras) {
      if (cam.row_source_index.size() > playable_frames) {
        cam.row_source_index.resize(playable_frames);
      }
    }
  }
}

}  // namespace trossen::io::backends
