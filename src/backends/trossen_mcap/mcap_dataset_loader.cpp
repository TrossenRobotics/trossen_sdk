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
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <regex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "trossen_sdk/data/record.hpp"
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
  // LeRobot stores fps as an integer, and rows sit at k / fps seconds, so a fractional rate
  // would leave info.json describing a different clock than the rows and videos use.
  if (!std::isfinite(alignment.fps) || alignment.fps <= 0.0 ||
      alignment.fps != std::floor(alignment.fps)) {
    std::cerr << "Error: alignment.fps must be a whole number above zero (LeRobot stores fps "
              << "as an integer; 30 is recommended), got " << alignment.fps << "\n";
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

  // ── Detect the joint count and mobile base; episode_action_dim() and episode_obs_dim()
  //    derive the row widths from them ──
  out.joints_per_stream = 0;
  for (const auto& [stream_id, messages] : messages_by_stream) {
    if (!messages.empty()) {
      out.joints_per_stream = static_cast<int>(messages[0].positions.size());
      break;
    }
  }
  // Every row is sized from that one count, so every sample of every arm must match it.
  for (const auto& [stream_id, messages] : messages_by_stream) {
    for (size_t i = 0; i < messages.size(); ++i) {
      const size_t joints = messages[i].positions.size();
      if (joints == 0 || static_cast<int>(joints) != out.joints_per_stream) {
        std::cerr << "Error: joint stream '" << stream_id << "' sample " << i << " has " << joints
                  << " positions; every arm must report " << out.joints_per_stream << "\n";
        return false;
      }
    }
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

  // Times at which rows are sampled, in MCAP log time.
  // TODO(shantanuparab-tr): fill with a uniform grid at alignment.fps over the window where
  // every stream has data (#358). Until then no rows are produced.
  std::vector<uint64_t> row_times;

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

  // Per-camera search cursor. Rows are visited in increasing time, so each cursor only
  // ever moves forward.
  std::map<std::string, size_t> camera_cursors;
  for (const auto& [camera_name, stamps] : camera_timestamps) camera_cursors[camera_name] = 0;

  // TODO(shantanuparab-tr): replace nearest_frame, find_closest_message and the mobile base
  // search below with one shared nearest-sample matcher that compares both neighbors for
  // every stream (#356).
  // Frame nearest `target`, or npos when the nearest is further away than the tolerance.
  // Unlike the joint matcher (which snaps to the last sample at or before the target),
  // this compares both neighbors, halving the worst-case pairing error from a full
  // frame period to half of one.
  auto nearest_frame = [&alignment](const std::vector<uint64_t>& stamps, uint64_t target,
                                    size_t& cursor) -> size_t {
    if (stamps.empty()) return std::numeric_limits<size_t>::max();
    while (cursor + 1 < stamps.size() && stamps[cursor + 1] <= target) ++cursor;
    size_t best = cursor;
    auto distance = [target](uint64_t ts) {
      return target > ts ? target - ts : ts - target;
    };
    if (cursor + 1 < stamps.size() && distance(stamps[cursor + 1]) < distance(stamps[cursor])) {
      best = cursor + 1;
    }
    return distance(stamps[best]) > alignment.tolerance_ns ? std::numeric_limits<size_t>::max()
                                                           : best;
  };

  // Records carry a dual (sec, nsec) Timestamp; the MCAP log time was written from the
  // realtime half, so that is the key every comparison below uses.
  auto log_ns = [](const data::RecordBase& rec) { return rec.ts.realtime.to_ns(); };
  // Distance between two timestamps. Subtracting the smaller from the larger keeps the
  // unsigned result from wrapping around.
  auto abs_diff_ns = [](uint64_t a, uint64_t b) { return a > b ? a - b : b - a; };

  auto find_closest_message = [&](const std::string& stream_id, uint64_t target_ts,
                                  size_t& idx) -> const data::JointStateRecord* {
    auto it = messages_by_stream.find(stream_id);
    if (it == messages_by_stream.end() || it->second.empty()) return nullptr;
    const auto& messages = it->second;
    if (idx >= messages.size()) return nullptr;
    while (idx + 1 < messages.size() && log_ns(messages[idx + 1]) <= target_ts) {
      ++idx;
    }
    if (abs_diff_ns(log_ns(messages[idx]), target_ts) > alignment.tolerance_ns) {
      return nullptr;
    }
    return &messages[idx];
  };

  // Appends one stream's enabled signal blocks in the order build_features() names them:
  // positions, then velocities, then efforts. Leaders pass positions only.
  auto append_joint_signals = [&](std::vector<double>& dst, const data::JointStateRecord& sample,
                                  bool with_velocity, bool with_effort) {
    // Each block contributes exactly joints_per_stream values: a short block is zero-filled
    // and a long one is truncated, so every row matches the width build_features() declares.
    const size_t n = static_cast<size_t>(out.joints_per_stream);
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

    std::vector<double> actions;
    bool have_all_leaders = true;
    for (const auto& leader_stream : out.leader_streams) {
      const auto* sample = find_closest_message(leader_stream, timestamp_ns,
                                                stream_indices[leader_stream]);
      if (sample) {
        append_joint_signals(actions, *sample, /*with_velocity=*/false, /*with_effort=*/false);
      } else {
        have_all_leaders = false;
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
        break;
      }
    }

    std::vector<double> base_values;
    if (channels.has_mobile_base) {
      // An empty base channel stops here, and the index check below drops the row.
      while (mobile_base_idx + 1 < mobile_base_messages.size() &&
             log_ns(mobile_base_messages[mobile_base_idx + 1]) <= timestamp_ns) {
        ++mobile_base_idx;
      }
      if (mobile_base_idx < mobile_base_messages.size() &&
          abs_diff_ns(log_ns(mobile_base_messages[mobile_base_idx]), timestamp_ns) <=
            alignment.tolerance_ns) {
        const auto& odom = mobile_base_messages[mobile_base_idx];
        base_values.push_back(odom.twist.linear_x);
        base_values.push_back(odom.twist.angular_z);
        if (signals.base_lateral_velocity) base_values.push_back(odom.twist.linear_y);
        if (signals.base_pose) {
          base_values.push_back(odom.pose.x);
          base_values.push_back(odom.pose.y);
          base_values.push_back(odom.pose.theta);
        }
      }
    }
    const bool have_base = !channels.has_mobile_base || !base_values.empty();

    if (!have_all_leaders || !have_all_followers || !have_base) {
      ++rows_skipped;
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
      if (frame_idx == std::numeric_limits<size_t>::max()) break;
      staged_camera_frames.push_back(frame_idx);
    }
    if (staged_camera_frames.size() != out.cameras.size()) {
      ++rows_skipped_no_frame;
      continue;
    }
    for (size_t c = 0; c < out.cameras.size(); ++c) {
      out.cameras[c].row_source_index.push_back(staged_camera_frames[c]);
    }

    if (channels.has_mobile_base) {
      actions.insert(actions.end(), base_values.begin(), base_values.end());
      observations.insert(observations.end(), base_values.begin(), base_values.end());
    }

    // Known bug: rows are stamped by position, so each dropped row makes every row after it
    // play one period early.
    // TODO(shantanuparab-tr): report mid-episode gaps and reject episodes over a gap limit
    // (#359).
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

  // How far each camera ends up from its rows: a large or growing offset means the camera
  // clock is drifting away from the row grid and is worth investigating.
  const std::ios::fmtflags cout_flags = std::cout.flags();
  const std::streamsize cout_precision = std::cout.precision();
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
  std::cout.flags(cout_flags);
  std::cout.precision(cout_precision);

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
