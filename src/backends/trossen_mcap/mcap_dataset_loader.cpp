/**
 * @file mcap_dataset_loader.cpp
 * @brief Implementation of the TrossenMCAP read + alignment path.
 *
 * This translation unit owns the single MCAP_IMPLEMENTATION definition for the SDK;
 * anything linking trossen_sdk must not define the macro.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// MCAP_IMPLEMENTATION must be defined before the reader header is included.
#define MCAP_IMPLEMENTATION
#include "mcap/reader.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"

#include "JointState.pb.h"
#include "Odometry2D.pb.h"

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
 * @brief Parse a protobuf message from an MCAP message payload.
 *
 * @param msg Message to fill.
 * @param payload MCAP message whose data holds the serialized @p msg.
 * @return False if the payload does not parse or is larger than protobuf's int size limit.
 */
template <typename Message>
bool parse_payload(Message& msg, const mcap::Message& payload) {
  if (payload.dataSize > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
  return msg.ParseFromArray(payload.data, static_cast<int>(payload.dataSize));
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
  const AlignmentOptions& alignment)
{
  // LeRobot stores fps as an integer, and rows sit at k / fps seconds, so a fractional rate
  // would leave info.json describing a different clock than the rows and videos use. Rows
  // are spaced in whole nanoseconds, so a rate above one row per nanosecond has no period.
  if (!std::isfinite(alignment.fps) || alignment.fps <= 0.0 ||
      alignment.fps != std::floor(alignment.fps) ||
      alignment.fps > static_cast<double>(data::S_TO_NS)) {
    std::cerr << "Error: alignment.fps must be a whole number from 1 to " << data::S_TO_NS
              << " (LeRobot stores fps as an integer; 30 is recommended), got " << alignment.fps
              << "\n";
    return false;
  }

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
      if (!parse_payload(odom_msg, messageView.message)) {
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
      continue;
    }

    auto joint_it = channels.joint_channels.find(messageView.channel->id);
    if (joint_it != channels.joint_channels.end()) {
      const std::string& stream_id = joint_it->second;
      trossen_sdk::msg::JointState js_msg;
      if (!parse_payload(js_msg, messageView.message)) {
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

  // ── Read the joint count from each stream's joint_names and detect the mobile base;
  //    episode_action_dim() and episode_obs_dim() derive the row widths from them ──
  out.joints_per_stream = 0;
  for (const auto& [stream_id, stream] : out.dataset_info.at("streams").items()) {
    const nlohmann::json names = stream.value("joint_names", nlohmann::json());
    const int joints = names.is_array() ? static_cast<int>(names.size()) : 0;
    if (joints == 0 || (out.joints_per_stream != 0 && joints != out.joints_per_stream)) {
      std::cerr << "Error: joint stream '" << stream_id << "' names " << joints
                << " joints in dataset_info; every arm stream must name the same nonzero count\n";
      return false;
    }
    out.joints_per_stream = joints;
  }
  out.has_mobile_base = channels.has_mobile_base;

  // ── Require joint data from at least one follower stream ──
  bool have_follower_data = false;
  for (const auto& stream : out.follower_streams) {
    auto it = messages_by_stream.find(stream);
    if (it != messages_by_stream.end() && !it->second.empty()) {
      have_follower_data = true;
      break;
    }
  }
  if (!have_follower_data) {
    std::cerr << "Error: No follower stream recorded any joint state messages\n";
    return false;
  }

  // Record the cameras present. Built before the row loop so each row can store the frame
  // it matched.
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

  // Times at which rows are sampled, in MCAP log time.
  // TODO(shantanuparab-tr): fill with a uniform grid at alignment.fps over the window where
  // every stream has data (#358). Until then no rows are produced.
  std::vector<uint64_t> row_times;

  const size_t max_rows = row_times.size();
  for (auto& cam : out.cameras) cam.row_source_index.reserve(max_rows);

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
