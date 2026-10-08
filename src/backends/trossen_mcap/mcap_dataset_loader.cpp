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
        out.dataset_info = nlohmann::json::parse(info_it->second);
        std::cout << "  [ok] Found MCAP dataset_info metadata\n";
        if (out.dataset_info.contains("robot_name")) {
          out.robot_name = out.dataset_info["robot_name"].get<std::string>();
          std::cout << "    Robot name from MCAP: " << out.robot_name << "\n";
        }
        // Per-episode task prompt embedded by the recorder (see
        // trossen_mcap_backend.cpp). Left empty when the recording predates
        // task-embedding or the operator set no task; the converter falls back
        // to its configured task_name in that case.
        if (out.dataset_info.contains("task")) {
          out.task_name = out.dataset_info["task"].get<std::string>();
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
