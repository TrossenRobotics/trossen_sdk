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
#include "FrameMeta.pb.h"
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

void StreamWindow::narrow(const std::string& name, uint64_t first, uint64_t last) {
  if (first >= start) {
    start = first;
    last_to_start = name;
  }
  if (last <= end) {
    end = last;
    first_to_end = name;
  }
}

bool StreamWindow::is_open() const {
  return end > start;
}

std::vector<uint64_t> row_grid(uint64_t start, uint64_t end, uint64_t period_ns) {
  const size_t row_count = static_cast<size_t>((end - start) / period_ns) + 1;
  std::vector<uint64_t> times;
  times.reserve(row_count);
  for (size_t k = 0; k < row_count; ++k) times.push_back(start + k * period_ns);
  return times;
}

double rows_to_ms(size_t rows, uint64_t row_period_ns) {
  return static_cast<double>(rows * row_period_ns) / 1e6;
}

GapSummary summarize_gaps(
  const std::vector<size_t>& gap_lengths, size_t grid_rows, uint64_t row_period_ns,
  const AlignmentOptions& alignment)
{
  GapSummary summary;
  for (const size_t length : gap_lengths) {
    summary.dropped_rows += length;
    summary.longest_gap_rows = std::max(summary.longest_gap_rows, length);
  }
  summary.dropped_fraction =
    static_cast<double>(summary.dropped_rows) / static_cast<double>(grid_rows);
  summary.gap_too_long =
    rows_to_ms(summary.longest_gap_rows, row_period_ns) > alignment.max_single_gap_ms;
  summary.too_many_gaps = summary.dropped_fraction > alignment.max_gap_fraction;
  return summary;
}

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
  // Rows are a whole number of nanoseconds apart, so a faster rate has no row period.
  if (alignment.fps > static_cast<double>(data::S_TO_NS)) {
    std::cerr << "Error: alignment.fps must be at most " << data::S_TO_NS
              << " (one row per nanosecond), got " << alignment.fps << "\n";
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
      const std::string stream_id =
        stream_id_from_topic(topic, trossen_mcap_defs::kOdometry2DTopicSuffix);
      std::cout << "    [ok] Found odometry stream for mobile robot: " << stream_id << "\n";
      // The Rivet example records its base as `trossen_base` on robot `trossen_rivet`.
      if (stream_id == "trossen_base" || out.robot_name == "trossen_rivet") {
        warn_rivet_unsupported();
      }
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
      static const std::regex camera_meta_topic_re(
        std::string("^") + trossen_mcap_defs::kCameraTopicPrefix + "(.+)" +
        trossen_mcap_defs::kCameraMetaTopicSuffix + "$");
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
      } else if (std::regex_match(topic, m, camera_meta_topic_re) && channel_ptr->schemaId != 0 &&
                 reader.schema(channel_ptr->schemaId) &&
                 reader.schema(channel_ptr->schemaId)->name == "trossen_sdk.msg.FrameMeta") {
        channels.camera_meta_channels[channel_id] = m[1].str();
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
  // Per camera, what the camera itself recorded for each frame, indexed by the frame's
  // position in the stream (FrameMeta.frame_index, the same index as camera_timestamps).
  // Two device times compare only when both carry the same clock.
  struct CaptureRecord {
    trossen_sdk::DeviceClock clock{trossen_sdk::DEVICE_CLOCK_NONE};
    uint64_t device_ns{0};
    std::optional<uint64_t> frame_number;
  };
  std::map<std::string, std::vector<CaptureRecord>> camera_capture;
  // Records as read, keyed by frame_index; moved into camera_capture once each camera's
  // frame count is known, so an index from a malformed message cannot size an allocation.
  std::map<std::string, std::map<size_t, CaptureRecord>> capture_by_index;

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
      continue;
    }

    auto meta_it = channels.camera_meta_channels.find(messageView.channel->id);
    if (meta_it != channels.camera_meta_channels.end()) {
      trossen_sdk::msg::FrameMeta meta;
      if (!parse_payload(meta, messageView.message)) {
        continue;
      }
      CaptureRecord record;
      record.clock = meta.ts().device_clock();
      if (record.clock != trossen_sdk::DEVICE_CLOCK_NONE) {
        record.device_ns = static_cast<uint64_t>(meta.ts().device().seconds()) * data::S_TO_NS +
                           static_cast<uint64_t>(meta.ts().device().nanos());
      }
      if (meta.has_device_frame_number()) record.frame_number = meta.device_frame_number();
      capture_by_index[meta_it->second][static_cast<size_t>(meta.frame_index())] = record;
    }
  }

  // A capture record whose frame_index is past the camera's last frame matches no image.
  for (auto& [camera_name, by_index] : capture_by_index) {
    const size_t frame_count = camera_timestamps[camera_name].size();
    auto& records = camera_capture[camera_name];
    records.resize(frame_count);
    size_t out_of_range = 0;
    for (auto& [index, record] : by_index) {
      if (index < frame_count) {
        records[index] = record;
      } else {
        ++out_of_range;
      }
    }
    if (out_of_range > 0) {
      std::ostringstream msg;
      msg << "Warning: " << camera_name << ": ignored " << out_of_range << " capture record(s) "
          << "with a frame_index past its " << frame_count << " frame(s)\n";
      std::cerr << msg.str();
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
    const auto step = first_backward_step(count, stamp_at);
    if (!step) return true;
    const size_t i = *step;
    std::cerr << "Error: " << name << " timestamps go backwards at message " << i << " ("
              << (stamp_at(i - 1) - stamp_at(i)) << " ns earlier than message " << (i - 1)
              << "). The recording clock stepped, so rows cannot be matched.\n";
    return false;
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

  StreamWindow window;

  for (const auto& stream_id : out.leader_streams) {
    const auto it = messages_by_stream.find(stream_id);
    if (it == messages_by_stream.end() || it->second.empty()) continue;
    window.narrow(stream_id, log_ns_of(it->second.front()), log_ns_of(it->second.back()));
  }
  for (const auto& stream_id : out.follower_streams) {
    const auto it = messages_by_stream.find(stream_id);
    if (it == messages_by_stream.end() || it->second.empty()) continue;
    window.narrow(stream_id, log_ns_of(it->second.front()), log_ns_of(it->second.back()));
  }
  for (const auto& cam : out.cameras) {
    const auto& stamps = camera_timestamps[cam.name];
    if (stamps.empty()) continue;
    window.narrow(cam.name, stamps.front(), stamps.back());
  }
  // Every row needs a base sample, so the base bounds the window like any other stream.
  if (channels.has_mobile_base) {
    if (mobile_base_messages.empty()) {
      std::cerr << "Error: the mobile base channel has no messages.\n";
      return false;
    }
    window.narrow("mobile base", log_ns_of(mobile_base_messages.front()),
                  log_ns_of(mobile_base_messages.back()));
  }

  // At least one joint stream has data, so the window always has a finite end. A window
  // that closed means one stream stopped before another started, and no row can hold a
  // sample from both.
  if (!window.is_open()) {
    std::cerr << "Error: no stretch of time has data from every stream. " << window.first_to_end
              << " ends " << static_cast<double>(window.start - window.end) / 1e6
              << " ms before " << window.last_to_start << " starts.\n";
    return false;
  }

  const uint64_t span_ns = window.end - window.start;
  const std::vector<uint64_t> row_times = row_grid(window.start, window.end, row_period_ns);
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
  // Each gap keeps the stream that had no sample in its first dropped row, every camera
  // that had no frame in any of its rows, and each such camera's frames either side of it.
  struct FrameSpan {
    size_t before{0};
    size_t after{0};
  };
  struct Gap {
    size_t first_row{0};
    size_t length{0};
    std::string missing_stream;
    std::set<size_t> missing_cameras;
    std::map<size_t, FrameSpan> camera_frames;
  };
  std::vector<Gap> gaps;
  Gap pending_gap{};
  auto note_skipped_row = [&](size_t row, const std::string& missing,
                              const std::vector<size_t>& missing_cameras) {
    if (out.frames.empty()) return;
    if (pending_gap.length == 0) {
      pending_gap = Gap{};
      pending_gap.first_row = row;
      pending_gap.missing_stream = missing;
    }
    pending_gap.missing_cameras.insert(missing_cameras.begin(), missing_cameras.end());
    ++pending_gap.length;
  };

  // Per-camera search cursor. Rows are visited in increasing time, so each cursor only
  // ever moves forward.
  std::map<std::string, size_t> camera_cursors;
  for (const auto& [camera_name, stamps] : camera_timestamps) camera_cursors[camera_name] = 0;

  // Index of the entry nearest `target`, or npos when the nearest is further away than the
  // tolerance. Both neighbors are compared: snapping only to the last entry at or before
  // the target biases every row backwards by up to a full sample period, and a bias does
  // not average out the way jitter does. Shared by joint, mobile base and camera streams
  // so they all pair rows the same way.
  auto nearest_index = [&alignment](size_t count, const auto& stamp_at, uint64_t target,
                                    size_t& cursor) -> size_t {
    return nearest_sample_index(count, stamp_at, target, alignment.tolerance_ns, cursor);
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

    // First stream without a sample within tolerance, if the row is dropped.
    std::string missing_stream;

    std::vector<double> actions;
    bool have_all_leaders = true;
    for (const auto& leader_stream : out.leader_streams) {
      const auto* sample = find_closest_message(leader_stream, timestamp_ns,
                                                stream_indices[leader_stream]);
      if (sample) {
        append_joint_signals(actions, *sample, /*with_velocity=*/false, /*with_effort=*/false);
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
      if (base_values.empty() && missing_stream.empty()) missing_stream = "mobile base";
    }
    const bool have_base = !channels.has_mobile_base || !base_values.empty();

    // Every camera must offer a frame within tolerance, else the row would pair a joint
    // state with a stale image. Selections are staged and only committed once the whole
    // row is known good, so a late miss can't leave the cameras half-filled.
    // Every camera is checked, even on a row already missing a joint or base sample, so a
    // gap knows each camera that had no frame in it.
    std::vector<size_t> staged_camera_frames;
    staged_camera_frames.reserve(out.cameras.size());
    std::vector<size_t> cameras_without_frame;
    for (size_t c = 0; c < out.cameras.size(); ++c) {
      const auto& cam = out.cameras[c];
      const size_t frame_idx = nearest_frame(camera_timestamps[cam.name], timestamp_ns,
                                             camera_cursors[cam.name]);
      if (frame_idx == std::numeric_limits<size_t>::max()) {
        if (missing_stream.empty()) missing_stream = cam.name;
        cameras_without_frame.push_back(c);
      }
      staged_camera_frames.push_back(frame_idx);
    }
    if (!have_all_leaders || !have_all_followers || !have_base) {
      ++rows_skipped;
      note_skipped_row(ref_idx, missing_stream, cameras_without_frame);
      continue;
    }
    if (!cameras_without_frame.empty()) {
      ++rows_skipped_no_frame;
      note_skipped_row(ref_idx, missing_stream, cameras_without_frame);
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
      // The camera's last frame before the gap is the one its previous kept row played.
      for (const size_t c : pending_gap.missing_cameras) {
        const auto& played = out.cameras[c].row_source_index;
        pending_gap.camera_frames[c] = FrameSpan{played[played.size() - 2], played.back()};
      }
      gaps.push_back(std::move(pending_gap));
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
    auto gap_ms = [row_period_ns](size_t rows) { return rows_to_ms(rows, row_period_ns); };
    std::vector<size_t> gap_lengths;
    gap_lengths.reserve(gaps.size());
    for (const auto& gap : gaps) gap_lengths.push_back(gap.length);
    const GapSummary summary =
      summarize_gaps(gap_lengths, row_times.size(), row_period_ns, alignment);
    const size_t gap_rows = summary.dropped_rows;
    const size_t longest_gap = summary.longest_gap_rows;
    const double gap_fraction = summary.dropped_fraction;
    const bool gap_too_long = summary.gap_too_long;
    const bool too_many_gaps = summary.too_many_gaps;

    // Formatted locally: episodes are loaded concurrently and std::cerr's flags are shared.
    std::ostringstream report;
    report << (gap_too_long || too_many_gaps ? "Error: " : "Warning: ") << gap_rows
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
        if (records[i].clock != trossen_sdk::DEVICE_CLOCK_NONE &&
            records[i].clock == records[i - 1].clock &&
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
    // What a camera's records say about the frames it lost between two of its frames.
    // kNoRecords: no frame counter and no device clock to tell. kUnknown: records exist but
    // cannot be compared (a counter that does not increase, as on a reset or wrap, or device
    // times on different clocks). kCounted: `lost` frames were never captured.
    struct FrameLoss {
      enum Kind { kNoRecords, kUnknown, kCounted } kind{kNoRecords};
      uint64_t lost{0};
    };
    // Frames that reach the host less than half a row period apart were delivered together.
    const uint64_t burst_ns =
      static_cast<uint64_t>(frame_duration_s * static_cast<double>(data::S_TO_NS) / 2.0);
    auto frames_lost_across = [&](const std::string& camera, const FrameSpan& span) {
      const auto& records = camera_capture[camera];
      const auto& stamps = camera_timestamps[camera];
      if (span.after >= records.size() || span.after <= span.before) return FrameLoss{};
      // A camera that recovers delivers its held frames in a burst, and the kept row after
      // the gap plays only one of them, so every frame of that burst is checked.
      size_t last = span.after;
      while (last + 1 < records.size() && stamps[last + 1] - stamps[last] < burst_ns) ++last;

      bool counters = true;
      bool any_counter = false;
      const trossen_sdk::DeviceClock clock = records[span.before].clock;
      bool clocks = clock != trossen_sdk::DEVICE_CLOCK_NONE;
      bool any_clock = false;
      for (size_t i = span.before; i <= last; ++i) {
        counters = counters && records[i].frame_number.has_value();
        any_counter = any_counter || records[i].frame_number.has_value();
        clocks = clocks && records[i].clock == clock;
        any_clock = any_clock || records[i].clock != trossen_sdk::DEVICE_CLOCK_NONE;
      }

      // Frames held back can reach the host out of capture order, so counters and device
      // times are compared sorted. Each must rise from the frame before the gap: a value at
      // or below it means a reset, a wrap or a repeat, and the loss cannot be counted.
      // Frames missing between consecutive values were never captured.
      auto lost_in = [](std::vector<uint64_t> values, double step) -> std::optional<uint64_t> {
        const uint64_t first = values.front();
        std::sort(values.begin(), values.end());
        if (values.front() != first) return std::nullopt;
        uint64_t lost = 0;
        for (size_t i = 1; i < values.size(); ++i) {
          if (values[i] <= values[i - 1]) return std::nullopt;
          const double steps = std::round(static_cast<double>(values[i] - values[i - 1]) / step);
          if (steps > 1.0) lost += static_cast<uint64_t>(steps) - 1;
        }
        return lost;
      };
      // The counter shows frames that were never captured, and the device clock shows a
      // pause in capture that a continuous counter does not, so the larger loss is kept.
      std::optional<uint64_t> by_counter;
      if (counters) {
        std::vector<uint64_t> numbers;
        for (size_t i = span.before; i <= last; ++i) numbers.push_back(*records[i].frame_number);
        by_counter = lost_in(std::move(numbers), 1.0);
      }
      std::optional<uint64_t> by_clock;
      const double period = clocks ? period_of(camera) : 0.0;
      if (period > 0.0) {
        std::vector<uint64_t> times;
        for (size_t i = span.before; i <= last; ++i) times.push_back(records[i].device_ns);
        by_clock = lost_in(std::move(times), period);
      }

      if (by_counter || by_clock) {
        return FrameLoss{FrameLoss::kCounted, std::max(by_counter.value_or(0),
                                                       by_clock.value_or(0))};
      }
      if (any_counter || any_clock) return FrameLoss{FrameLoss::kUnknown, 0};
      return FrameLoss{};
    };

    std::set<std::string> stalled_cameras;
    std::set<std::string> delayed_cameras;
    for (const auto& gap : gaps) {
      report << "    - " << gap.length << " row(s) (" << gap_ms(gap.length) << " ms) at "
             << static_cast<double>(row_times[gap.first_row] - row_times.front()) /
                  static_cast<double>(data::S_TO_NS)
             << " s, " << gap.missing_stream << " had no sample";
      for (const auto& [c, span] : gap.camera_frames) {
        const std::string& camera = out.cameras[c].name;
        const FrameLoss loss = frames_lost_across(camera, span);
        report << "; " << camera;
        if (loss.kind == FrameLoss::kNoRecords) {
          report << " has no capture record to tell a stall from a delivery delay";
          stalled_cameras.insert(camera);
        } else if (loss.kind == FrameLoss::kUnknown) {
          report << " frame counter or device clock changed across the gap, so frame loss "
                 << "is unknown";
        } else if (loss.lost > 0) {
          report << " lost " << loss.lost << " frame(s)";
          stalled_cameras.insert(camera);
        } else {
          report << " captured every frame, delivery was delayed";
          delayed_cameras.insert(camera);
        }
      }
      report << "\n";
    }
    for (const auto& camera : stalled_cameras) delayed_cameras.erase(camera);

    // A delivery delay is not a camera fault: the frames exist, and only matching rows on
    // host arrival time drops them.
    if (!delayed_cameras.empty()) {
      report << "  Delivery delay, not a stall (";
      for (auto it = delayed_cameras.begin(); it != delayed_cameras.end(); ++it) {
        report << (it == delayed_cameras.begin() ? "" : ", ") << *it;
      }
      report << "): the camera captured every frame, but they reached the host late, so "
             << "rows matched on arrival time were dropped.\n";
    }

    // A camera stall is a recording fault. Dropping its rows drops them for every camera,
    // so each stream-copied video that kept recording lags its joints from the gap on, and
    // no conversion step can restore the frames the stalled camera never captured.
    if (!stalled_cameras.empty()) {
      report << "  BAD EPISODE: camera stall during recording (";
      for (auto it = stalled_cameras.begin(); it != stalled_cameras.end(); ++it) {
        report << (it == stalled_cameras.begin() ? "" : ", ") << *it;
      }
      report << "). Rows were dropped for every camera, so the video of any camera that "
             << "kept recording lags its joints after each gap. Conversion cannot repair "
             << "this; discard or re-record the episode.\n";
    }
    if (gap_too_long) {
      report << "  The longest gap, " << gap_ms(longest_gap) << " ms, is over the "
             << alignment.max_single_gap_ms << " ms limit; rejecting the episode.\n";
    }
    if (too_many_gaps) {
      report << "  Dropped rows are over the " << alignment.max_gap_fraction * 100.0
             << "% limit; rejecting the episode.\n";
    }
    std::cerr << report.str();
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
    // Formatted locally: episodes are loaded concurrently and std::cout's flags are shared.
    std::ostringstream line;
    line << "    - " << cam.name << ": " << stamps.size() << " frames, pairing offset mean "
         << std::fixed << std::setprecision(1)
         << (sum_abs_ms / static_cast<double>(out.frames.size())) << " ms, worst " << worst_ms
         << " ms\n";
    std::cout << line.str();
  }

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
  // Episodes are prepared concurrently, so each message is built locally and written in
  // one call.
  for (const auto& cam : ep.cameras) {
    auto it = video_streams.find(cam.name);
    if (it == video_streams.end() || it->second.frame_count == 0) continue;
    if (cam.row_source_index.empty()) continue;

    const size_t offset = video_start_offset(cam);
    if (offset > 0 && !start_offset_applied) {
      std::ostringstream msg;
      msg << "\n  WARNING: episode " << ep.episode_index << " " << cam.name
          << " video is offset by " << offset
          << " frame(s). Row 0 was matched to frame " << offset << ", but the copied "
          << "video starts at frame 0, so every image lags its joint state by "
          << offset << " source frame(s). This format cannot start a copied video "
          << "mid-stream; convert with trossen_mcap_to_lerobot_v3 for offset-correct "
          << "video.\n\n";
      std::cerr << msg.str();
    }

    size_t first_mismatch = cam.row_source_index.size();
    for (size_t row = 1; row < cam.row_source_index.size(); ++row) {
      if (cam.row_source_index[row] != offset + row) {
        first_mismatch = row;
        break;
      }
    }
    // Row n plays frame played_start + n.
    const size_t played_start = start_offset_applied ? offset : 0;

    // Frames after the last row's are never played, so they do not move any row's image.
    const size_t covered = ep.frames.size() + played_start;
    if (it->second.frame_count > covered) {
      std::ostringstream msg;
      msg << "  Episode " << ep.episode_index << " " << cam.name << ": "
          << (it->second.frame_count - covered)
          << " frame(s) after the last row are not played\n";
      std::cout << msg.str();
    }
    if (first_mismatch == cam.row_source_index.size()) continue;

    std::ostringstream msg;
    msg << "Warning: episode " << ep.episode_index << " " << cam.name
        << " video is not row aligned. Row " << first_mismatch
        << " matched frame " << cam.row_source_index[first_mismatch]
        << ", but one frame per row from row 0 gives frame " << (offset + first_mismatch)
        << ". Copying the stream as recorded plays frame " << (played_start + first_mismatch)
        << " there, so the images drift from the joint data.\n";
    std::cerr << msg.str();
  }
}

void clamp_episode_to_video_frame_counts(
  AlignedEpisode& ep, const std::map<std::string, CameraVideoStream>& video_streams,
  bool start_offset_applied) {
  size_t playable_frames = std::numeric_limits<size_t>::max();
  std::string shortest_camera;
  for (const auto& cam : ep.cameras) {
    auto it = video_streams.find(cam.name);
    if (it == video_streams.end() || it->second.frame_count == 0) continue;
    // Frames before the one matched to row 0 are never played once the start is moved.
    const size_t skipped = start_offset_applied ? video_start_offset(cam) : 0;
    const size_t frames = it->second.frame_count;
    const size_t playable = frames > skipped ? frames - skipped : 0;
    if (playable < playable_frames) {
      playable_frames = playable;
      shortest_camera = cam.name;
    }
  }
  if (playable_frames < ep.frames.size()) {
    std::ostringstream msg;
    msg << "  Episode " << ep.episode_index << ": dropped the last "
        << (ep.frames.size() - playable_frames) << " of " << ep.frames.size()
        << " row(s); " << shortest_camera << " video has " << playable_frames
        << " playable frame(s)\n";
    std::cout << msg.str();
    ep.frames.resize(playable_frames);
    for (auto& cam : ep.cameras) {
      if (cam.row_source_index.size() > playable_frames) {
        cam.row_source_index.resize(playable_frames);
      }
    }
  }
}

}  // namespace trossen::io::backends
