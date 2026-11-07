/**
 * @file mcap_dataset_loader.cpp
 * @brief Implementation of the TrossenMCAP read + alignment path.
 *
 * This translation unit owns the single MCAP_IMPLEMENTATION definition for the SDK;
 * anything linking trossen_sdk must not define the macro.
 */

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// MCAP_IMPLEMENTATION must be defined before the reader header is included.
#define MCAP_IMPLEMENTATION
#include "mcap/reader.hpp"

#include "trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp"
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

/**
 * @brief Map each channel to the stream dataset_info describes at its topic.
 *
 * The recorder writes each joint stream's role and topic, each camera's topic, size, rate,
 * depth flag and storage, and the mobile base's topic and velocity names. Every described
 * stream must have a channel; channels at other topics are left unmapped.
 *
 * @param reader Open reader for the recording.
 * @param info The recording's dataset_info.
 * @param channels Filled with the joint, camera and mobile base channels.
 * @param leaders Filled with the leader stream ids, sorted.
 * @param followers Filled with the follower stream ids, sorted.
 * @return False, with a message on stderr, if a description is incomplete, a described stream
 *   has no channel, or the recording lacks a leader or a follower.
 */
bool map_described_channels(mcap::McapReader& reader, const nlohmann::json& info,
                            McapChannelMap& channels, std::vector<std::string>& leaders,
                            std::vector<std::string>& followers) {
  using nlohmann::json;
  std::map<std::string, std::string> joint_topics;   // topic -> stream id
  std::map<std::string, std::string> camera_topics;  // topic -> camera name
  std::string base_topic;

  // Held in locals: a range-for over items() of a temporary would read a destroyed object.
  const json streams = info.value("streams", json::object());
  const json cameras = info.value("cameras", json::object());
  for (const auto& [id, stream] : streams.items()) {
    const json role = stream.value("role", json());
    const json topic = stream.value("topic", json());
    const bool described = topic.is_string() &&
                           (role == trossen_mcap_defs::kLeaderRole ||
                            role == trossen_mcap_defs::kFollowerRole);
    if (!described) {
      std::cerr << "Error: joint stream '" << id << "' has no dataset_info topic and role of '"
                << trossen_mcap_defs::kLeaderRole << "' or '" << trossen_mcap_defs::kFollowerRole
                << "'\n";
      return false;
    }
    (role == trossen_mcap_defs::kLeaderRole ? leaders : followers).push_back(id);
    joint_topics[topic.get<std::string>()] = id;
  }

  for (const auto& [name, camera] : cameras.items()) {
    const json storage = camera.value("storage", json());
    const bool described =
      camera.value("topic", json()).is_string() &&
      (storage == trossen_mcap_defs::kRawStorage || storage == trossen_mcap_defs::kH264Storage ||
       storage == trossen_mcap_defs::kH265Storage);
    if (!described) {
      std::cerr << "Error: camera '" << name << "' has no dataset_info entry with a topic and "
                << "a storage of '" << trossen_mcap_defs::kRawStorage << "', '"
                << trossen_mcap_defs::kH264Storage << "' or '" << trossen_mcap_defs::kH265Storage
                << "'\n";
      return false;
    }
    camera_topics[camera["topic"].get<std::string>()] = name;
    channels.camera_storage[name] = storage.get<std::string>();
  }

  if (info.contains("mobile_base")) {
    if (!info["mobile_base"].value("topic", json()).is_string()) {
      std::cerr << "Error: the mobile base has no dataset_info topic\n";
      return false;
    }
    base_topic = info["mobile_base"]["topic"].get<std::string>();
  }

  for (const auto& [channel_id, channel] : reader.channels()) {
    const std::string& topic = channel->topic;
    if (const auto it = joint_topics.find(topic); it != joint_topics.end()) {
      channels.joint_channels[channel_id] = it->second;
    } else if (const auto cam = camera_topics.find(topic); cam != camera_topics.end()) {
      channels.camera_channels[channel_id] = cam->second;
    } else if (!base_topic.empty() && topic == base_topic) {
      channels.mobile_base_channel_id = channel_id;
      channels.has_mobile_base = true;
    }
  }

  // A stream the recorder described but never wrote a message for has no channel.
  std::vector<std::string> silent;
  for (const auto& [topic, id] : joint_topics) {
    if (std::none_of(channels.joint_channels.begin(), channels.joint_channels.end(),
                     [&](const auto& entry) { return entry.second == id; })) {
      silent.push_back(id);
    }
  }
  for (const auto& [topic, name] : camera_topics) {
    if (std::none_of(channels.camera_channels.begin(), channels.camera_channels.end(),
                     [&](const auto& entry) { return entry.second == name; })) {
      silent.push_back(name);
    }
  }
  if (!base_topic.empty() && !channels.has_mobile_base) silent.push_back("mobile base");
  if (!silent.empty()) {
    std::cerr << "Error: described stream(s) recorded no messages: " << join_ids(silent) << "\n";
    return false;
  }

  if (leaders.empty() || followers.empty()) {
    std::cerr << "Error: A recording needs both leader and follower arm streams. Leaders: ["
              << join_ids(leaders) << "], followers: [" << join_ids(followers) << "]\n";
    return false;
  }
  // Sorted, so the action and observation columns come out in the same order for every
  // recording from a rig, whatever order its producers registered in.
  std::sort(leaders.begin(), leaders.end());
  std::sort(followers.begin(), followers.end());
  return true;
}

}  // namespace

bool load_aligned_episode(
  const std::string& mcap_file,
  int episode_index,
  AlignedEpisode& out,
  McapChannelMap& channels,
  [[maybe_unused]] const DatasetSignalOptions& signals,
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
  std::string format_version;
  for (auto it = range.first; it != range.second; ++it) {
    mcap::Record raw_record;
    auto rs = mcap::McapReader::ReadRecord(*data_source, it->second.offset, &raw_record);
    if (!rs.ok()) continue;

    mcap::Metadata meta_record;
    rs = mcap::McapReader::ParseMetadata(raw_record, &meta_record);
    if (!rs.ok()) continue;

    auto version_it = meta_record.metadata.find(trossen_mcap_defs::kFormatVersionKey);
    if (version_it != meta_record.metadata.end()) format_version = version_it->second;

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
  }

  if (format_version != trossen_mcap_defs::kRecordingFormatVersion) {
    std::cerr << "Error: '" << mcap_file << "' has recording format "
              << (format_version.empty() ? std::string("of no version") : format_version)
              << "; this SDK reads format " << trossen_mcap_defs::kRecordingFormatVersion << "\n";
    return false;
  }

  // Left empty when the operator set no task; the converter then uses its configured one.
  if (!out.task_name.empty()) {
    std::cout << "    Task from MCAP: " << out.task_name << "\n";
  }

  // ── PHASE 2: Map channels to the streams dataset_info describes ──
  if (!map_described_channels(reader, out.dataset_info, channels, out.leader_streams,
                              out.follower_streams)) {
    return false;
  }
  std::cout << "\n  [ok] Detected configuration:\n";
  std::cout << "    Leader streams (" << out.leader_streams.size()
            << "): " << join_ids(out.leader_streams) << "\n";
  std::cout << "    Follower streams (" << out.follower_streams.size()
            << "): " << join_ids(out.follower_streams) << "\n";
  if (!channels.camera_channels.empty()) {
    std::cout << "  Found " << channels.camera_channels.size() << " camera channel(s)\n";
  }

  // TODO(shantanuparab-tr): implement the nearest-timestamp row alignment (#319).

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
  // TODO(shantanuparab-tr): implement the per-row camera frame decode and write (#307).
  return false;
}

bool extract_camera_video(
  [[maybe_unused]] const std::string& mcap_file,
  [[maybe_unused]] const McapChannelMap& channels,
  [[maybe_unused]] const std::function<std::filesystem::path(const std::string& camera_name)>&
    dir_for,
  [[maybe_unused]] std::map<std::string, CameraVideoStream>& out_streams)
{
  // TODO(shantanuparab-tr): implement the Annex B elementary stream extraction (#308).
  return false;
}

void clamp_episode_to_video_frame_counts(
  [[maybe_unused]] AlignedEpisode& ep,
  [[maybe_unused]] const std::map<std::string, CameraVideoStream>& video_streams) {
  // TODO(shantanuparab-tr): implement the trim to the shortest video-mode camera (#308).
}

}  // namespace trossen::io::backends
