/**
 * @file mcap_dataset_loader.cpp
 * @brief Implementation of the TrossenMCAP read + alignment path.
 *
 * This translation unit owns the single MCAP_IMPLEMENTATION definition for the SDK;
 * anything linking trossen_sdk must not define the macro.
 */

#define MCAP_IMPLEMENTATION
#include "mcap/reader.hpp"

#include "trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"

namespace trossen::io::backends {

static_assert(std::is_same_v<McapChannelId, mcap::ChannelId>,
              "McapChannelId must match the MCAP library's channel id type");

namespace {

/**
 * @brief mcap reader callback: log a recoverable parsing issue and keep reading.
 *
 * @param problem Status the reader reports for the damaged record.
 */
void on_problem(const mcap::Status& problem) {
  std::cerr << "Warning: MCAP parsing issue: " << problem.message << "\n";
}

/**
 * @brief Stream id in front of a topic suffix, without the leading `/` a topic may carry.
 *
 * @param topic Full topic name, e.g. `follower_left/joints/state`.
 * @param suffix Suffix @p topic is known to end with.
 * @return The stream id, e.g. `follower_left`.
 */
std::string stream_id_from_topic(const std::string& topic, std::string_view suffix) {
  std::string stream_id = topic.substr(0, topic.size() - suffix.size());
  if (!stream_id.empty() && stream_id.front() == '/') stream_id.erase(0, 1);
  return stream_id;
}

/**
 * @brief Copy a string field out of dataset_info, warning when it holds another type.
 *
 * @param dataset_info Parsed dataset_info JSON.
 * @param key Field to read.
 * @param[out] value Set to the field's value when it is a string; left unchanged otherwise.
 * @return True if the field was present and a string.
 */
bool read_string_field(
  const nlohmann::json& dataset_info, const char* key, std::string& value)
{
  auto it = dataset_info.find(key);
  if (it == dataset_info.end()) return false;
  if (!it->is_string()) {
    std::cerr << "  Warning: dataset_info field '" << key << "' is not a string; ignoring it\n";
    return false;
  }
  value = it->get<std::string>();
  return true;
}

/**
 * @brief Join stream ids into one comma-separated list for log messages.
 *
 * @param ids Stream ids to join.
 * @return The ids separated by ", ", or an empty string when there are none.
 */
std::string join_ids(const std::vector<std::string>& ids) {
  std::string joined;
  for (const auto& id : ids) {
    if (!joined.empty()) joined += ", ";
    joined += id;
  }
  return joined;
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
    std::cerr << "Error: Failed to open MCAP file '" << mcap_file << "'\n";
    return false;
  }

  mcap::McapReader reader;
  auto status = reader.open(input);
  if (!status.ok()) {
    std::cerr << "Error: Failed to parse MCAP file '" << mcap_file << "': " << status.message
              << "\n";
    return false;
  }

  auto summary_status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
  if (!summary_status.ok()) {
    std::cerr << "Error: Failed to read the summary of MCAP file '" << mcap_file
              << "': " << summary_status.message << "\n";
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

    // The recorder writes the task prompt as its own field of this record.
    auto task_it = meta_record.metadata.find(trossen_mcap_defs::kTaskDescriptionKey);
    if (task_it != meta_record.metadata.end() && !task_it->second.empty()) {
      out.task_name = task_it->second;
    }

    auto info_it = meta_record.metadata.find(trossen_mcap_defs::kDatasetInfoKey);
    if (info_it == meta_record.metadata.end()) continue;

    try {
      out.dataset_info = nlohmann::json::parse(info_it->second);
    } catch (const nlohmann::json::parse_error& e) {
      std::cerr << "  Warning: Failed to parse dataset_info metadata: " << e.what() << "\n";
      continue;
    }
    std::cout << "  [ok] Found MCAP dataset_info metadata\n";

    if (read_string_field(out.dataset_info, "robot_name", out.robot_name)) {
      std::cout << "    Robot name from MCAP: " << out.robot_name << "\n";
    }
    // Older recordings carry the task inside dataset_info instead.
    if (out.task_name.empty()) {
      read_string_field(out.dataset_info, "task", out.task_name);
    }
  }

  // Left empty when the operator set no task; the converter then uses its configured one.
  if (!out.task_name.empty()) {
    std::cout << "    Task from MCAP: " << out.task_name << "\n";
  }

  // ── PHASE 2: Map channels to streams; detect leader/follower arms by name ──
  std::vector<std::string> detected_leader_streams;
  std::vector<std::string> detected_follower_streams;

  // Joint streams named with neither role. Any one of them rejects the recording.
  std::vector<std::string> unnamed_streams;

  // Every topic seen, so a detection failure below can say what the recording did hold.
  std::vector<std::string> all_topics;

  // Topic of the odometry stream already mapped, named if a second one appears.
  std::string mobile_base_topic;

  std::cout << "  Available channels:\n";
  for (const auto& [channel_id, channel_ptr] : reader.channels()) {
    const std::string& topic = channel_ptr->topic;
    std::cout << "    - Topic: '" << topic << "'\n";
    all_topics.push_back(topic);

    if (topic.ends_with(trossen_mcap_defs::kOdometry2DTopicSuffix)) {
      // The episode holds a single mobile base, so a second odometry stream has no place.
      if (channels.has_mobile_base) {
        std::cerr << "Error: Two odometry streams found ('" << mobile_base_topic << "' and '"
                  << topic << "'); a recording may hold only one mobile base\n";
        return false;
      }
      channels.mobile_base_channel_id = channel_id;
      channels.has_mobile_base = true;
      mobile_base_topic = topic;
      std::cout << "    [ok] Found odometry stream for mobile robot: "
                << stream_id_from_topic(topic, trossen_mcap_defs::kOdometry2DTopicSuffix)
                << "\n";
      continue;
    }

    if (topic.ends_with(trossen_mcap_defs::kJointStateTopicSuffix)) {
      std::string stream_id =
        stream_id_from_topic(topic, trossen_mcap_defs::kJointStateTopicSuffix);
      if (trossen_mcap_defs::is_leader_stream_id(stream_id)) {
        detected_leader_streams.push_back(stream_id);
        std::cout << "    [ok] Detected leader stream: " << stream_id << "\n";
      } else if (trossen_mcap_defs::is_follower_stream_id(stream_id)) {
        detected_follower_streams.push_back(stream_id);
        std::cout << "    [ok] Detected follower stream: " << stream_id << "\n";
      } else {
        unnamed_streams.push_back(stream_id);
      }
      channels.joint_channels[channel_id] = std::move(stream_id);
      continue;
    }

    // Topic format: /cameras/<camera_name>/image, with the camera name as the capture
    // group. Built from the topic constants so the writer and reader cannot drift apart.
    {
      static const std::regex camera_topic_re(
        std::string("^") + trossen_mcap_defs::kCameraTopicPrefix + "(.+)" +
        trossen_mcap_defs::kImageTopicSuffix + "$");
      std::smatch m;
      if (std::regex_match(topic, m, camera_topic_re)) {
        // The name becomes a directory and file name in the output, so one that could
        // point outside it is not a camera.
        const std::string name = m[1].str();
        if (name == "." || name == ".." || name.find_first_of("/\\") != std::string::npos) {
          std::cerr << "Warning: skipping camera topic '" << topic
                    << "': the camera name must be a single path component\n";
          continue;
        }
        channels.camera_channels[channel_id] = name;
      }
    }
  }

  // Naming the topics that were present separates an empty recording from one whose
  // topics simply do not follow the convention.
  if (channels.joint_channels.empty()) {
    std::cerr << "Error: No joint state channels found in MCAP file\n";
    if (all_topics.empty()) {
      std::cerr << "  The recording has no channels at all.\n";
    } else {
      std::cerr << "  Expected a topic ending in '" << trossen_mcap_defs::kJointStateTopicSuffix
                << "'. The recording holds:\n";
      for (const auto& topic : all_topics) std::cerr << "    - " << topic << "\n";
    }
    return false;
  }

  // The stream names alone decide which arms become the action and which the observation,
  // so a name carrying neither role is an error rather than a guess.
  if (!unnamed_streams.empty()) {
    std::cerr << "Error: Joint stream(s) named neither '" << trossen_mcap_defs::kLeaderStreamToken
              << "[_<side>]' nor '" << trossen_mcap_defs::kFollowerStreamToken
              << "[_<side>]': " << join_ids(unnamed_streams) << "\n";
    return false;
  }

  if (detected_leader_streams.empty() || detected_follower_streams.empty()) {
    std::cerr << "Error: A recording needs both leader and follower arm streams. Leaders: ["
              << join_ids(detected_leader_streams) << "], followers: ["
              << join_ids(detected_follower_streams) << "]\n";
    return false;
  }

  // Sorted, so the action and observation columns come out in the same order for every
  // recording from a rig, whatever order its producers registered in.
  std::sort(detected_leader_streams.begin(), detected_leader_streams.end());
  std::sort(detected_follower_streams.begin(), detected_follower_streams.end());
  out.leader_streams = std::move(detected_leader_streams);
  out.follower_streams = std::move(detected_follower_streams);
  std::cout << "\n  [ok] Detected configuration:\n";
  std::cout << "    Leader streams (" << out.leader_streams.size()
            << "): " << join_ids(out.leader_streams) << "\n";
  std::cout << "    Follower streams (" << out.follower_streams.size()
            << "): " << join_ids(out.follower_streams) << "\n";

  if (!channels.camera_channels.empty()) {
    std::cout << "  Found " << channels.camera_channels.size() << " camera channel(s)\n";
  }

  // TODO(shantanuparab-tr): implement the nearest-timestamp row alignment.

  return true;
}

bool extract_camera_images(
  [[maybe_unused]] const std::string& mcap_file,
  [[maybe_unused]] const McapChannelMap& channels,
  [[maybe_unused]] const AlignedEpisode& episode,
  [[maybe_unused]] const std::function<std::filesystem::path(const std::string& camera_name)>&
    dir_for,
  [[maybe_unused]] std::map<std::string, size_t>& out_counts)
{
  // TODO(shantanuparab-tr): implement the per-row camera frame decode and write.
  return false;
}

bool extract_camera_video(
  [[maybe_unused]] const std::string& mcap_file,
  [[maybe_unused]] const McapChannelMap& channels,
  [[maybe_unused]] const std::function<std::filesystem::path(const std::string& camera_name)>&
    dir_for,
  [[maybe_unused]] std::map<std::string, CameraVideoStream>& out_streams)
{
  // TODO(shantanuparab-tr): implement the Annex B elementary stream extraction.
  return false;
}

void clamp_episode_to_video_frame_counts(
  [[maybe_unused]] AlignedEpisode& ep,
  [[maybe_unused]] const std::map<std::string, CameraVideoStream>& video_streams) {
  // TODO(shantanuparab-tr): implement the trim to the shortest video-mode camera.
}

}  // namespace trossen::io::backends
