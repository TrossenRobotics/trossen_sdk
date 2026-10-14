/**
 * @file lerobot_v2_format_utils.hpp
 * @brief LeRobot v2.1 on-disk layout helpers: file naming and per-episode metadata.
 *
 * v2.1 writes one parquet and one video per episode, appends episode/task/stats
 * metadata to jsonl files, and updates a single info.json in place after every
 * episode:
 *
 *     <dataset_root>/
 *       data/
 *         chunk-000/
 *           episode_000000.parquet      one file per episode
 *       videos/
 *         chunk-000/
 *           <camera_key>/
 *             episode_000000.mp4        one file per episode per camera
 *       images/
 *         <camera_key>/                 only when frames are kept unencoded
 *           episode_000000/
 *             image_000000.jpg
 *       meta/
 *         info.json                     rewritten after every episode
 *         episodes.jsonl                one line appended per episode
 *         episodes_stats.jsonl          one line appended per episode
 *         tasks.jsonl                   one line per distinct task
 *
 * v3.0 aggregates many episodes into shared, size-rolled files and tracks
 * metadata as parquet, so it does not use these helpers.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2__LEROBOT_V2_FORMAT_UTILS_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2__LEROBOT_V2_FORMAT_UTILS_HPP_

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_constants.hpp"

namespace trossen::io::backends {

// ============================================================================
// LeRobotV2 Naming and Formatting Utilities
// ============================================================================

/**
 * @brief Format an episode folder name
 *
 * @param episode_index Episode index (0-based)
 * @return Formatted episode folder name (e.g., "episode_000000")
 */
inline std::string format_episode_folder(int episode_index) {
  std::ostringstream oss;
  oss << "episode_" << std::setfill('0') << std::setw(6) << episode_index;
  return oss.str();
}

/**
 * @brief Format a chunk directory name
 *
 * @param chunk_index Chunk index (0-based)
 * @return Formatted chunk directory name (e.g., "chunk-000")
 */
inline std::string format_chunk_dir(int chunk_index) {
  std::ostringstream oss;
  oss << "chunk-" << std::setfill('0') << std::setw(3) << chunk_index;
  return oss.str();
}

/**
 * @brief Format an episode parquet filename
 *
 * @param episode_index Episode index (0-based)
 * @return Formatted parquet filename (e.g., "episode_000000.parquet")
 */
inline std::string format_episode_parquet(int episode_index) {
  std::ostringstream oss;
  oss << "episode_" << std::setfill('0') << std::setw(6) << episode_index << ".parquet";
  return oss.str();
}

/**
 * @brief Format an image filename
 *
 * @param frame_index Frame index (0-based)
 * @return Formatted image filename (e.g., "image_000000.jpg")
 */
inline std::string format_image_filename(int frame_index) {
  std::ostringstream oss;
  oss << "image_" << std::setfill('0') << std::setw(6) << frame_index << ".jpg";
  return oss.str();
}

/**
 * @brief Format a video filename
 *
 * @param episode_index Episode index (0-based)
 * @return Formatted video filename (e.g., "episode_000000.mp4")
 */
inline std::string format_video_filename(int episode_index) {
  std::ostringstream oss;
  oss << "episode_" << std::setfill('0') << std::setw(6) << episode_index << ".mp4";
  return oss.str();
}

/**
 * @brief Format a depth image filename (16-bit PNG)
 *
 * @param frame_index Frame index (0-based)
 * @return Formatted depth image filename (e.g., "image_000000.png")
 */
inline std::string format_depth_filename(int frame_index) {
  std::ostringstream oss;
  oss << "image_" << std::setfill('0') << std::setw(6) << frame_index << ".png";
  return oss.str();
}

// ============================================================================
// LeRobotV2 Metadata Utility Functions
// ============================================================================

/**
 * @brief Create initial info.json file for a new dataset
 *
 * @param meta_dir Path to the meta directory
 * @param robot_name Name of the robot
 * @param features JSON object containing feature definitions
 * @param fps Frames per second (default: 30)
 * @param codebase_version Codebase version string (default: "v2.1")
 * @param chunk_size Episodes per data chunk directory (default: 1000)
 * @return true on success, false on failure
 */
inline bool create_initial_info_json(
    const std::filesystem::path& meta_dir,
    const std::string& robot_name,
    const nlohmann::ordered_json& features,
    int fps = 30,
    const std::string& codebase_version = "v2.1",
    int chunk_size = 1000) {
  namespace fs = std::filesystem;

  fs::path info_path = meta_dir / JSON_INFO;

  // Don't overwrite if it already exists
  if (fs::exists(info_path)) {
    return true;
  }

  nlohmann::ordered_json info_json;

  // Basic metadata
  info_json["codebase_version"] = codebase_version;
  info_json["trossen_subversion"] = TROSSEN_SUBVERSION;
  info_json["robot_type"] = robot_name;
  info_json["fps"] = fps;

  // Counters
  info_json["total_episodes"] = 0;
  info_json["total_frames"] = 0;
  info_json["total_videos"] = 0;
  info_json["total_tasks"] = 1;
  info_json["total_chunks"] = 1;
  info_json["chunks_size"] = chunk_size;

  // Data splits (initially empty)
  info_json["splits"]["train"] = "0:0";

  // Feature definitions
  info_json["features"] = features;

  // Path templates
  info_json["data_path"] = DATA_PATH_META;
  info_json["video_path"] = VIDEO_PATH_META;

  // Write to file
  std::ofstream info_file(info_path);
  if (!info_file.is_open()) {
    std::cerr << "Error: Failed to create " << info_path << " for writing\n";
    return false;
  }

  info_file << info_json.dump(4);
  info_file.close();
  return true;
}

/**
 * @brief Update info.json with episode counts, frames, videos, and train split
 *
 * @param meta_dir Path to the meta directory
 * @param episode_frame_count Number of frames in this episode
 * @param num_videos Number of videos in this episode
 * @return true on success, false on failure
 */
inline bool update_info_json(
    const std::filesystem::path& meta_dir,
    int episode_frame_count,
    int num_videos) {
  namespace fs = std::filesystem;

  fs::path info_path = meta_dir / JSON_INFO;
  nlohmann::ordered_json info_json;

  // Load existing info.json if it exists
  if (fs::exists(info_path)) {
    std::ifstream info_file(info_path);
    if (info_file.is_open()) {
      info_file >> info_json;
      info_file.close();
    }
  } else {
    std::cerr << "Warning: info.json does not exist at " << info_path << "\n";
    return false;
  }

  // Update episode count
  int total_episodes = info_json.value("total_episodes", 0) + 1;
  info_json["total_episodes"] = total_episodes;

  // Update total videos
  info_json["total_videos"] = info_json.value("total_videos", 0) + num_videos;

  // Update total frames
  info_json["total_frames"] = info_json.value("total_frames", 0) + episode_frame_count;

  // Recompute total_chunks = ceil(total_episodes / chunks_size)
  int chunks_size = info_json.value("chunks_size", 1000);
  info_json["total_chunks"] = (total_episodes + chunks_size - 1) / chunks_size;

  // Update splits (simple logic: all episodes go to train)
  // TODO(shantanuparab-tr): establish how `splits` is meant to partition a dataset into
  // train and evaluation sets, and what consumes it. Every dataset produced so far puts
  // all episodes in "train", so the split has never been exercised.
  std::string train_split = info_json["splits"].value("train", "0:0");
  size_t colon_pos = train_split.find(':');
  int train_start = 0;
  int train_end = 0;
  if (colon_pos != std::string::npos) {
    train_start = std::stoi(train_split.substr(0, colon_pos));
    train_end = std::stoi(train_split.substr(colon_pos + 1));
  }
  train_end += 1;  // add one episode to train
  info_json["splits"]["train"] = std::to_string(train_start) + ":" + std::to_string(train_end);

  // Write back to info.json
  std::ofstream info_file(info_path);
  if (!info_file.is_open()) {
    std::cerr << "Error: Failed to open " << info_path << " for writing\n";
    return false;
  }

  info_file << info_json.dump(4);
  info_file.close();
  return true;
}

/**
 * @brief Append an episode entry to episodes.jsonl
 *
 * @param meta_dir Path to the meta directory
 * @param episode_index Episode index
 * @param task_name Task name
 * @param episode_length Number of frames in the episode
 * @return true on success, false on failure
 */
inline bool write_episode_entry(
    const std::filesystem::path& meta_dir,
    int episode_index,
    const std::string& task_name,
    int episode_length) {
  namespace fs = std::filesystem;

  // Append mode: episodes.jsonl holds one JSON object per line, one line per episode.
  fs::path episodes_path = meta_dir / JSONL_EPISODES;
  std::ofstream episodes_file(episodes_path, std::ios::app);

  if (!episodes_file.is_open()) {
    std::cerr << "Error: Failed to open " << episodes_path << " for writing\n";
    return false;
  }

  nlohmann::json episode_entry;
  episode_entry["episode_index"] = episode_index;
  episode_entry["tasks"] = nlohmann::json::array({task_name});
  episode_entry["length"] = episode_length;

  episodes_file << episode_entry.dump() << "\n";
  episodes_file.close();
  return true;
}

/**
 * @brief Write a task entry to tasks.jsonl if it doesn't exist
 *
 * @param meta_dir Path to the meta directory
 * @param task_index Task index
 * @param task_name Task name
 * @return true on success, false on failure
 */
inline bool write_task_entry(
    const std::filesystem::path& meta_dir,
    int task_index,
    const std::string& task_name) {
  namespace fs = std::filesystem;

  fs::path tasks_path = meta_dir / JSONL_TASKS;

  // Only create if it doesn't exist
  if (fs::exists(tasks_path)) {
    return true;  // Already exists, nothing to do
  }

  std::ofstream tasks_file(tasks_path);
  if (!tasks_file.is_open()) {
    std::cerr << "Error: Failed to open " << tasks_path << " for writing\n";
    return false;
  }

  nlohmann::ordered_json task_entry;
  task_entry["task_index"] = task_index;
  task_entry["task"] = task_name;

  tasks_file << task_entry.dump() << "\n";
  tasks_file.close();
  return true;
}

/**
 * @brief Remove the last JSONL entry matching a given episode_index
 *
 * Only removes the last line if its "episode_index" field matches the
 * expected index. Returns true (no-op) if the last entry doesn't match,
 * preventing accidental deletion of a previous episode's metadata.
 *
 * @param file_path Path to the JSONL file
 * @param expected_episode_index Episode index that must match for removal
 * @return true on success or no-op, false on I/O failure
 */
inline bool remove_last_jsonl_line(
    const std::filesystem::path& file_path,
    int expected_episode_index) {
  namespace fs = std::filesystem;
  if (!fs::exists(file_path)) return true;

  std::ifstream in(file_path);
  if (!in.is_open()) return false;

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) lines.push_back(line);
  }
  in.close();

  if (lines.empty()) return true;

  // Only remove if the last entry's episode_index matches
  try {
    auto entry = nlohmann::json::parse(lines.back());
    if (entry.value("episode_index", -1) != expected_episode_index) {
      return true;  // no-op: last entry belongs to a different episode
    }
  } catch (const std::exception&) {
    return false;  // malformed JSON
  }

  lines.pop_back();

  std::ofstream out(file_path, std::ios::trunc);
  if (!out.is_open()) return false;
  for (const auto& l : lines) {
    out << l << "\n";
  }
  out.close();
  return true;
}

/**
 * @brief Revert info.json counters after discarding an episode
 *
 * Decrements total_episodes, total_frames, total_videos, and adjusts
 * the train split range. Requires the frame count that was recorded for
 * the discarded episode.
 *
 * @param meta_dir Path to the meta directory
 * @param episode_frame_count Number of frames in the discarded episode
 * @param num_videos Number of video streams in the discarded episode
 * @return true on success, false on failure
 */
inline bool revert_info_json(
    const std::filesystem::path& meta_dir,
    int episode_frame_count,
    int num_videos) {
  namespace fs = std::filesystem;

  fs::path info_path = meta_dir / JSON_INFO;
  if (!fs::exists(info_path)) return true;

  nlohmann::ordered_json info_json;
  {
    std::ifstream info_file(info_path);
    if (!info_file.is_open()) return false;
    info_file >> info_json;
  }

  int total_episodes = std::max(0, info_json.value("total_episodes", 0) - 1);
  info_json["total_episodes"] = total_episodes;
  info_json["total_videos"] = std::max(0, info_json.value("total_videos", 0) - num_videos);
  info_json["total_frames"] = std::max(0, info_json.value("total_frames", 0) - episode_frame_count);

  int chunks_size = info_json.value("chunks_size", 1000);
  info_json["total_chunks"] = std::max(1, (total_episodes + chunks_size - 1) / chunks_size);

  // Adjust train split end (guard against missing/malformed "splits")
  if (info_json.contains("splits") && info_json["splits"].is_object() &&
      info_json["splits"].contains("train")) {
    try {
      std::string train_split = info_json["splits"].value("train", "0:0");
      size_t colon_pos = train_split.find(':');
      if (colon_pos != std::string::npos) {
        int train_start = std::stoi(train_split.substr(0, colon_pos));
        int train_end = std::max(train_start, std::stoi(train_split.substr(colon_pos + 1)) - 1);
        info_json["splits"]["train"] =
            std::to_string(train_start) + ":" + std::to_string(train_end);
      }
    } catch (const std::exception&) {
      // Malformed split value, leave unchanged
    }
  }

  std::ofstream out(info_path);
  if (!out.is_open()) return false;
  out << info_json.dump(4);
  out.close();
  return true;
}

/**
 * @brief Append episode statistics to episodes_stats.jsonl
 *
 * @param meta_dir Path to the meta directory
 * @param episode_index Episode index
 * @param num_frames Number of frames in the episode
 * @param stats Optional statistics JSON object (if empty, only basic stats are written)
 * @return true on success, false on failure
 */
inline bool write_episode_stats(
    const std::filesystem::path& meta_dir,
    int episode_index,
    int num_frames,
    const nlohmann::json& stats = nlohmann::json()) {
  namespace fs = std::filesystem;

  fs::path stats_path = meta_dir / JSONL_EPISODE_STATS;
  std::ofstream stats_file(stats_path, std::ios::app);

  if (!stats_file.is_open()) {
    std::cerr << "Error: Failed to open " << stats_path << " for writing\n";
    return false;
  }

  nlohmann::json stats_entry;
  stats_entry["episode_index"] = episode_index;

  if (stats.empty() || !stats.is_object()) {
    // Write minimal stats with just num_frames
    stats_entry["num_frames"] = num_frames;
  } else {
    // Write full stats
    stats_entry["stats"] = stats;
  }

  stats_file << stats_entry.dump() << "\n";
  stats_file.close();
  return true;
}

/**
 * @brief Write episode statistics with computed stats object
 *
 * @param meta_dir Path to the meta directory
 * @param episode_index Episode index
 * @param stats Statistics JSON object containing feature statistics
 * @return true on success, false on failure
 */
inline bool write_episode_stats_with_data(
    const std::filesystem::path& meta_dir,
    int episode_index,
    const nlohmann::json& stats) {
  namespace fs = std::filesystem;

  fs::path stats_path = meta_dir / JSONL_EPISODE_STATS;
  std::ofstream stats_file(stats_path, std::ios::app);

  if (!stats_file.is_open()) {
    std::cerr << "Error: Failed to open " << stats_path << " for writing\n";
    return false;
  }

  nlohmann::ordered_json stats_entry;
  stats_entry["episode_index"] = episode_index;
  stats_entry["stats"] = stats;

  stats_file << stats_entry.dump() << "\n";
  stats_file.close();
  return true;
}

/**
 * @brief Write all metadata files for an episode
 *
 * This is a convenience function that calls the individual metadata writing functions.
 *
 * @param meta_dir Path to the meta directory
 * @param episode_index Episode index
 * @param task_name Task name
 * @param task_index Task index (default: 0)
 * @param episode_length Number of frames in the episode
 * @param num_videos Number of videos in the episode
 * @param stats Optional statistics JSON object
 * @return true on success, false on failure
 */
inline bool write_episode_metadata(
    const std::filesystem::path& meta_dir,
    int episode_index,
    const std::string& task_name,
    int task_index,
    int episode_length,
    int num_videos,
    const nlohmann::json& stats = nlohmann::json()) {
  // Write task entry (if doesn't exist)
  if (!write_task_entry(meta_dir, task_index, task_name)) {
    return false;
  }

  // Write episode entry
  if (!write_episode_entry(meta_dir, episode_index, task_name, episode_length)) {
    return false;
  }

  // Write episode stats
  if (!write_episode_stats(meta_dir, episode_index, episode_length, stats)) {
    return false;
  }

  // Update info.json
  if (!update_info_json(meta_dir, episode_length, num_videos)) {
    return false;
  }

  return true;
}

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2__LEROBOT_V2_FORMAT_UTILS_HPP_
