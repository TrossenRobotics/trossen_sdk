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
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <opencv2/opencv.hpp>

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"
#include "trossen_sdk/utils/depth_quantization.hpp"

#include "JointState.pb.h"
#include "Odometry2D.pb.h"
#include "CompressedVideo.pb.h"
#include "RawImage.pb.h"

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
      // A base channel with no messages yields npos here, so the row is dropped below.
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
  const std::string& mcap_file,
  const McapChannelMap& channels,
  const AlignedEpisode& episode,
  const std::function<std::filesystem::path(const std::string& camera_name)>& dir_for,
  std::map<std::string, size_t>& out_counts)
{
  namespace fs = std::filesystem;
  out_counts.clear();
  if (channels.camera_channels.empty()) {
    return true;
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

  // A depth channel records the size of one raw unit, in meters, as its `depth_scale_m`
  // metadata.
  std::map<std::string, std::string> depth_scale_text;
  for (const auto& [channel_id, camera_name] : channels.camera_channels) {
    const auto channel = image_reader.channel(channel_id);
    if (!channel) continue;
    auto scale_it = channel->metadata.find("depth_scale_m");
    if (scale_it != channel->metadata.end()) depth_scale_text[camera_name] = scale_it->second;
  }

  // Millimeters per raw depth unit, per camera, resolved on its first depth frame.
  std::map<std::string, double> mm_per_unit;

  // Invert the per-row selection: source frame index → the rows that matched it. A frame
  // matched by two consecutive rows is decoded once and written under both row numbers.
  std::map<std::string, std::map<size_t, std::vector<size_t>>> rows_by_source;
  std::map<std::string, size_t> expected_counts;
  for (const auto& cam : episode.cameras) {
    // One selected frame per row; any other count would write images under the wrong rows.
    if (cam.row_source_index.size() != episode.frames.size()) {
      std::cerr << "Error: camera '" << cam.name << "' selects " << cam.row_source_index.size()
                << " frames for " << episode.frames.size() << " rows\n";
      return false;
    }
    auto& mapping = rows_by_source[cam.name];
    for (size_t row = 0; row < cam.row_source_index.size(); ++row) {
      mapping[cam.row_source_index[row]].push_back(row);
    }
    expected_counts[cam.name] = cam.row_source_index.size();
  }

  std::map<std::string, fs::path> camera_dirs;
  for (const auto& [camera_name, expected] : expected_counts) {
    camera_dirs[camera_name] = dir_for(camera_name);
    out_counts[camera_name] = 0;
  }

  // Arrival counter per camera, mirroring the indices load_aligned_episode() assigned.
  std::map<std::string, size_t> source_indices;

  // The reader skips a damaged chunk and keeps going, so any read problem fails the
  // extraction rather than leaving the images short of what the rows matched.
  bool read_failed = false;
  const auto on_read_problem = [&read_failed](const mcap::Status& problem) {
    on_problem(problem);
    read_failed = true;
  };

  size_t images_saved = 0;
  for (const auto& messageView : image_reader.readMessages(on_read_problem)) {
    auto it = channels.camera_channels.find(messageView.channel->id);
    if (it == channels.camera_channels.end()) continue;
    const std::string& camera_name = it->second;
    const size_t source_idx = source_indices[camera_name]++;

    // Only frames some row matched are decoded; the rest are read past.
    const auto mapping_it = rows_by_source.find(camera_name);
    if (mapping_it == rows_by_source.end()) continue;
    const auto& mapping = mapping_it->second;
    auto rows_it = mapping.find(source_idx);
    if (rows_it == mapping.end()) continue;
    const std::vector<size_t>& target_rows = rows_it->second;

    foxglove::RawImage raw_image;
    if (!parse_payload(raw_image, messageView.message)) {
      std::cerr << "Error: Failed to parse RawImage message for " << camera_name << " source frame "
                << source_idx << " (needed by row " << target_rows.front() << ")\n";
      return false;
    }

    const std::string& encoding = raw_image.encoding();
    int cv_type = -1;
    if (encoding == "bgr8" || encoding == "rgb8" || encoding == "8UC3") {
      cv_type = CV_8UC3;
    } else if (encoding == "rgba8" || encoding == "bgra8") {
      cv_type = CV_8UC4;
    } else if (encoding == "mono8" || encoding == "8UC1") {
      cv_type = CV_8UC1;
    } else if (encoding == "mono16" || encoding == "16UC1" || encoding == "depth16") {
      cv_type = CV_16UC1;
    } else {
      std::cerr << "Error: Unsupported encoding '" << encoding << "' for " << camera_name
                << " source frame " << source_idx << "\n";
      return false;
    }

    // 16-bit depth is kept losslessly as PNG; a JPEG would clip every value above 255.
    const bool depth_png = cv_type == CV_16UC1;

    // LeRobot reads integer depth as millimeters, so raw units are rescaled to mm. A camera
    // that recorded no scale is taken to be in DEFAULT_DEPTH_SCALE_M units.
    if (depth_png && mm_per_unit.count(camera_name) == 0) {
      double scale_m = trossen::utils::DEFAULT_DEPTH_SCALE_M;
      auto text_it = depth_scale_text.find(camera_name);
      if (text_it == depth_scale_text.end()) {
        std::cerr << "Warning: " << camera_name << " records no depth_scale_m; assuming "
                  << scale_m << " m per unit\n";
      } else if (auto parsed = trossen::utils::parse_depth_scale_m(text_it->second)) {
        scale_m = *parsed;
      } else {
        std::cerr << "Error: " << camera_name << " has an invalid depth_scale_m '"
                  << text_it->second << "'\n";
        return false;
      }
      mm_per_unit[camera_name] = scale_m * trossen::utils::MM_PER_METER;
    }

    // The payload must hold every row the header describes, else the view below reads past
    // the end of the message, and cv::Mat needs a step that is a whole number of channel
    // elements (2 bytes for 16-bit), else its constructor throws.
    const size_t width = raw_image.width();
    const size_t height = raw_image.height();
    const size_t row_bytes = width * static_cast<size_t>(CV_ELEM_SIZE(cv_type));
    const size_t step = raw_image.step() == 0 ? row_bytes : raw_image.step();
    if (width == 0 || height == 0 || step < row_bytes ||
        step % static_cast<size_t>(CV_ELEM_SIZE1(cv_type)) != 0 ||
        raw_image.data().size() < step * (height - 1) + row_bytes) {
      std::cerr << "Error: " << camera_name << " source frame " << source_idx << " holds "
                << raw_image.data().size() << " bytes for a " << raw_image.width() << "x"
                << raw_image.height() << " '" << encoding << "' image\n";
      return false;
    }

    // A view over the message's bytes, not a copy; it is written out before raw_image goes
    // out of scope. cv::Mat takes a non-const pointer but nothing writes through it.
    cv::Mat image(raw_image.height(), raw_image.width(), cv_type,
                  const_cast<char*>(raw_image.data().data()), step);

    // Converted into a new buffer, so the message bytes under the view stay untouched.
    cv::Mat bgr;
    if (encoding == "rgb8") {
      cv::cvtColor(image, bgr, cv::COLOR_RGB2BGR);
    } else if (encoding == "rgba8") {
      cv::cvtColor(image, bgr, cv::COLOR_RGBA2BGR);
    } else if (encoding == "bgra8") {
      cv::cvtColor(image, bgr, cv::COLOR_BGRA2BGR);
    }
    if (!bgr.empty()) image = bgr;

    // convertTo rounds to the nearest millimeter and saturates at 65535. It writes a new
    // buffer, so the message bytes under the view stay untouched.
    cv::Mat depth_mm;
    if (depth_png && mm_per_unit[camera_name] != 1.0) {
      image.convertTo(depth_mm, CV_16UC1, mm_per_unit[camera_name]);
      image = depth_mm;
    }

    // Depth is saved as lossless 16-bit PNG and color as JPEG. The LeRobot v3 writer later
    // picks its video encode path from this extension (.png → gray12le HEVC, .jpg → AV1).
    std::vector<int> compression_params =
      depth_png ? std::vector<int>{cv::IMWRITE_PNG_COMPRESSION, DEFAULT_DEPTH_PNG_COMPRESSION_LEVEL}
                : std::vector<int>{cv::IMWRITE_JPEG_QUALITY, DEFAULT_JPEG_QUALITY};

    // Frames are numbered by row, not by arrival, so the encoded video lines up with the
    // parquet rows one-for-one.
    for (size_t row : target_rows) {
      char namebuf[32];
      std::snprintf(namebuf, sizeof(namebuf), depth_png ? "image_%06zu.png" : "image_%06zu.jpg",
                    row);
      fs::path image_path = camera_dirs[camera_name] / namebuf;
      if (!cv::imwrite(image_path.string(), image, compression_params)) {
        std::cerr << "Error: Failed to save image: " << image_path.string() << "\n";
        return false;
      }
      ++images_saved;
      out_counts[camera_name]++;
    }
  }

  if (read_failed) {
    std::cerr << "Error: " << mcap_file << " has unreadable data; camera images would be "
              << "incomplete\n";
    return false;
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
  // Handed to the caller only once every stream is complete.
  std::map<std::string, CameraVideoStream> streams;

  // A failed extraction removes the stream files it started, so no partial stream is left.
  const auto fail = [&outputs, &streams]() {
    for (auto& [camera_name, out] : outputs) out.close();
    for (const auto& [camera_name, stream] : streams) {
      if (stream.annexb_path.empty()) continue;
      std::error_code ec;
      fs::remove(stream.annexb_path, ec);
    }
    return false;
  };

  // The reader skips a damaged chunk and keeps going, which would drop frames from the
  // middle of a copied stream, so any read problem fails the extraction.
  bool read_failed = false;
  const auto on_read_problem = [&read_failed](const mcap::Status& problem) {
    on_problem(problem);
    read_failed = true;
  };

  for (const auto& messageView : reader.readMessages(on_read_problem)) {
    auto it = channels.camera_channels.find(messageView.channel->id);
    if (it == channels.camera_channels.end()) continue;
    // Only video channels here; RawImage cameras belong to extract_camera_images().
    if (!messageView.schema || messageView.schema->name != "foxglove.CompressedVideo") {
      continue;
    }
    const std::string& camera_name = it->second;
    const auto found = streams.find(camera_name);
    const size_t frame_index = found == streams.end() ? 0 : found->second.frame_count;

    // One message is one frame. Skipping one would shift every later frame against the
    // arrival indices the rows were matched to, so a frame that does not parse is fatal.
    foxglove::CompressedVideo msg;
    if (!parse_payload(msg, messageView.message)) {
      std::cerr << "Error: Failed to parse CompressedVideo for " << camera_name << " frame "
                << frame_index << "\n";
      return fail();
    }
    // An empty payload would count as a frame without adding one to the stream.
    if (msg.data().empty()) {
      std::cerr << "Error: CompressedVideo for " << camera_name << " frame " << frame_index
                << " has no data\n";
      return fail();
    }

    auto& stream = streams[camera_name];
    if (stream.frame_count == 0) {
      stream.format = msg.format();
      // ffmpeg infers the demuxer from the extension for raw elementary streams. Only
      // h264 and h265 payloads are Annex B, so no other codec can be copied this way.
      std::string ext;
      if (stream.format == "h264") {
        ext = ".h264";
      } else if (stream.format == "h265") {
        ext = ".hevc";
      } else {
        std::cerr << "Error: " << camera_name << " has video format '" << stream.format
                  << "'; only h264 and h265 can be remuxed\n";
        return fail();
      }
      const fs::path path = dir_for(camera_name) / (camera_name + ext);
      outputs[camera_name].open(path, std::ios::binary);
      if (!outputs[camera_name]) {
        std::cerr << "Error: cannot open " << path.string() << " for writing\n";
        return fail();
      }
      // Recorded only once the file is open, so fail() removes only files this call opened.
      stream.annexb_path = path;
    } else if (msg.format() != stream.format) {
      // A camera that changed codec mid-episode cannot be remuxed as one stream.
      std::cerr << "Error: " << camera_name << " changed format from " << stream.format << " to "
                << msg.format() << " mid-episode\n";
      return fail();
    }

    outputs[camera_name].write(msg.data().data(), static_cast<std::streamsize>(msg.data().size()));
    ++stream.frame_count;
  }

  if (read_failed) {
    std::cerr << "Error: " << mcap_file << " has unreadable data; video streams would be "
              << "incomplete\n";
    return fail();
  }

  for (auto& [camera_name, out] : outputs) {
    out.close();
    if (!out) {
      std::cerr << "Error: failed writing video stream for " << camera_name << "\n";
      return fail();
    }
  }

  out_streams = std::move(streams);
  if (!out_streams.empty()) {
    std::cout << "  [ok] Extracted " << out_streams.size() << " compressed video stream(s)";
    for (const auto& [name, s] : out_streams) {
      std::cout << " " << name << "(" << s.format << ", " << s.frame_count << "f)";
    }
    std::cout << "\n";
  }
  return true;
}

void clamp_episode_to_video_frame_counts(
  AlignedEpisode& ep, const std::map<std::string, CameraVideoStream>& video_streams) {
  size_t min_video_frames = std::numeric_limits<size_t>::max();
  for (const auto& [name, vs] : video_streams) {
    if (vs.frame_count > 0) min_video_frames = std::min(min_video_frames, vs.frame_count);
  }
  if (min_video_frames < ep.frames.size()) {
    ep.frames.resize(min_video_frames);
    for (auto& cam : ep.cameras) {
      if (cam.row_source_index.size() > min_video_frames) {
        cam.row_source_index.resize(min_video_frames);
      }
    }
  }
}

}  // namespace trossen::io::backends
