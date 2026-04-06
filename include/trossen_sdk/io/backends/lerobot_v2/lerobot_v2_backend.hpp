/**
 * @file lerobot_v2_backend.hpp
 * @brief LeRobotV2 backend: writes joint states to Parquet and images to directory tree. Converts
 * images to videos per source using FFmpeg after recording.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2_BACKEND_HPP
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2_BACKEND_HPP

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/io/backend.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_constants.hpp"
#include "trossen_sdk/configuration/types/backends/lerobot_v2_backend_config.hpp"

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
// LeRobotV2 Statistics and Image Utility Functions
// ============================================================================

/**
 * @brief Compute statistics for a ListArray
 *
 * @param list_array Shared pointer to the ListArray
 * @return JSON object containing the computed statistics
 */
nlohmann::ordered_json compute_list_stats(
  const std::shared_ptr<arrow::ListArray> &list_array);

/**
 * @brief Compute statistics for a flat array
 *
 * @param array Shared pointer to the flat array
 * @return JSON object containing the computed statistics
 */
nlohmann::ordered_json compute_flat_stats(const std::shared_ptr<arrow::Array> &array);

/**
 * @brief Compute statistics for a FixedSizeListArray
 *
 * @param fixed_list_array Shared pointer to the FixedSizeListArray
 * @return JSON object containing the computed statistics
 */
nlohmann::ordered_json compute_fixed_size_list_stats(
  const std::shared_ptr<arrow::FixedSizeListArray> &fixed_list_array);

/**
 * @brief Compute statistics for a set of images
 *
 * @param images Vector of OpenCV Mat objects representing the images
 * @return FeatureStats object containing the computed statistics
 */
nlohmann::ordered_json compute_image_stats(const std::vector<cv::Mat> &images);

/**
 * @brief Sample a set of images from a list of image paths
 *
 * @param image_paths Vector of filesystem paths to the images
 * @return Vector of OpenCV Mat objects representing the sampled images
 */
std::vector<cv::Mat> sample_images(
  const std::vector<std::filesystem::path> &image_paths);

/**
 * @brief Automatically downsample an image to a target size
 *
 * @param img OpenCV Mat object representing the image
 * @param target_size Target size for the downsampled image (default is 150)
 * @param max_threshold Maximum threshold for downsampling (default is 300)
 * @return Downsampled OpenCV Mat object
 */
cv::Mat auto_downsample(
  const cv::Mat &img,
  int target_size = 150,
  int max_threshold = 300);

/**
 * @brief Sample indices for selecting images from a dataset
 *
 * @param dataset_len Length of the dataset
 * @param min_samples Minimum number of samples to select (default is 100)
 * @param max_samples Maximum number of samples to select (default is 10000)
 * @param power Power factor for sampling distribution (default is 0.75)
 * @return Vector of sampled indices
 */
std::vector<int> sample_indices(
  int dataset_len,
  int min_samples = 100,
  int max_samples = 10000,
  float power = 0.75f);

// ============================================================================
// LeRobotV2 Metadata Utility Functions
// ============================================================================

/**
 * @brief Create a common scalar feature definition
 *
 * @param dtype Data type (e.g., "int64", "float32")
 * @return JSON object representing the feature
 */
inline nlohmann::ordered_json create_scalar_feature(const std::string& dtype) {
  nlohmann::ordered_json feature;
  feature["dtype"] = dtype;
  feature["shape"] = nlohmann::json::array({1});
  feature["names"] = nlohmann::json::array();
  return feature;
}

/**
 * @brief Add standard LeRobotV2 metadata features (timestamp, indices, etc.)
 *
 * @param features JSON object to add features to (modified in place)
 */
inline void add_standard_metadata_features(nlohmann::ordered_json& features) {
  features["timestamp"] = create_scalar_feature("float32");
  features["frame_index"] = create_scalar_feature("int64");
  features["episode_index"] = create_scalar_feature("int64");
  features["index"] = create_scalar_feature("int64");
  features["task_index"] = create_scalar_feature("int64");
}

/**
 * @brief Create initial info.json file for a new dataset
 *
 * @param meta_dir Path to the meta directory
 * @param robot_name Name of the robot
 * @param features JSON object containing feature definitions
 * @param fps Frames per second (default: 30)
 * @param codebase_version Codebase version string (default: "v2.1")
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
      // Malformed split value — leave unchanged
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
 * @brief Generate a README.md dataset card for HuggingFace Hub compatibility
 *
 * Creates a README.md with YAML frontmatter (license, task_categories, tags, configs)
 * and a markdown body embedding the full info.json content.
 *
 * @param dataset_root Path to the dataset root directory (containing meta/, data/, etc.)
 * @return true on success, false on failure
 */
inline bool generate_dataset_readme(const std::filesystem::path& dataset_root) {
  namespace fs = std::filesystem;

  fs::path readme_path = dataset_root / "README.md";
  fs::path info_path = dataset_root / METADATA_DIR / JSON_INFO;

  // Read info.json content for embedding in the README
  std::string info_json_str = "[More Information Needed]";
  if (fs::exists(info_path)) {
    std::ifstream info_file(info_path);
    if (info_file.is_open()) {
      nlohmann::ordered_json info_json;
      info_file >> info_json;
      info_file.close();
      info_json_str = info_json.dump(4);
    }
  }

  std::ofstream readme_file(readme_path);
  if (!readme_file.is_open()) {
    std::cerr << "Error: Failed to create " << readme_path << " for writing\n";
    return false;
  }

  readme_file
    << "---\n"
    << "license: apache-2.0\n"
    << "task_categories:\n"
    << "- robotics\n"
    << "tags:\n"
    << "- LeRobot\n"
    << "configs:\n"
    << "- config_name: default\n"
    << "  data_files: data/*/*.parquet\n"
    << "---\n"
    << "\n"
    << "This dataset was created using [LeRobot](https://github.com/huggingface/lerobot).\n"
    << "\n"
    << "## Dataset Description\n"
    << "\n"
    << "Converted from TrossenMCAP format using the Trossen SDK "
    << "`trossen_mcap_to_lerobot_v2` tool.\n"
    << "\n"
    << "## Dataset Structure\n"
    << "\n"
    << "[meta/info.json](meta/info.json):\n"
    << "\n"
    << "```json\n"
    << info_json_str << "\n"
    << "```\n";

  readme_file.close();
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

// ============================================================================
// LeRobotV2Backend Class
// ============================================================================

/**
 * @brief Backend producing a simple on-disk layout for datasets.
 *
 * Layout (root = uri provided to open()):
 *   root/
 *     <repo-id>/
 *         <dataset-name>/
 *             data/
 *                 <chunk-id>/
 *                     <episode-id>.<data-format>
 *             meta/
 *                 <metadata-files>
 *             images/
 *                 <chunk-id>/
 *                    <source-id>/
 *                       <episode-id>/
 *                           <image-id>.<image-format>
 *             videos/
 *                 <chunk-id>/
 *                     <source-id>/
 *                         <video-id>.<video-format>
 */
class LeRobotV2Backend : public io::Backend {
public:
  /**
   * @enum Image queue drop policy when full
   */
  enum class DropPolicy {
    /// @brief Drop newest incoming image
    DropNewest,

    /// @brief Drop oldest image in queue to make room for new one
    DropOldest,

    /// @brief Block until space is available (not implemented)
    // Block
  };

  /**
   * @brief Construct a LeRobotV2Backend
   *
   * @param metadata Vector of producer metadata to include in info.json
   */
  explicit LeRobotV2Backend(
    ProducerMetadataList metadata);

  /**
   * @brief Prepare backend for a new episode
   *
   * @param output_path Base output path for this episode
   * @param episode_index Zero-based episode index
   * @param dataset_id Dataset identifier
   * @param repository_id Repository identifier (unused)
   */
  void preprocess_episode() override;

  /**
   * @brief Open a LeRobotV2 logging destination
   *
   * @return true on success
   */
  bool open() override;

  /**
   * @brief Write a single record
   *
   * @param record Record to write
   */
  void write(const data::RecordBase& record) override;

  /**
   * @brief Write a batch of records
   *
   * @param records Records to write
   */
  void write_batch(std::span<const data::RecordBase* const> records) override;

  /**
   * @brief Flush any buffered data
   */
  void flush() override;

  /**
   * @brief Close the backend
   */
  void close() override;

  /**
   * @brief Discard episode data and delete parquet file + image directories
   */
  void discard_episode() override;

  /**
   * @brief Add metadata to be written to info.json
   *
   * @param md Metadata to add
   */
  void write_metadata();

  /**
   * @brief Convert recorded images to videos using FFmpeg
   */
  void convert_to_videos() const;

  /**
   * @brief Compute and print statistics about the recorded data
   */
  void compute_statistics() const;

  /**
   * @brief Print statistics in a tabular format
   *
   * @param stats JSON object containing the statistics
   */
  void print_stats_table(const nlohmann::ordered_json& stats) const;

  /**
   * @brief Scan directory for existing episode files and return next index
   *
   * @return Next episode index (max_found + 1, or 0 if none found)
   */
   uint32_t scan_existing_episodes() override;

  /**
   * @brief Image encoding statistics
   */
  struct ImageEncodeStats {
    /// @brief Total images enqueued
    uint64_t enqueued{0};

    /// @brief Total images successfully encoded & written
    uint64_t written{0};

    /// @brief Total images dropped due to queue full
    uint64_t dropped{0};

    /// @brief Accumulated image encode time (ns)
    uint64_t encode_time_ns_acc{0};

    /// @brief Maximum single image encode time (ns)
    uint64_t encode_time_ns_max{0};

    /// @brief Accumulated queue wait time before encode (ns)
    uint64_t queue_wait_ns_acc{0};

    /// @brief Max queue wait time (ns)
    uint64_t queue_wait_ns_max{0};

    /// @brief High water mark of image queue length
    size_t queue_high_water{0};

    /// @brief Average image queue length (samples taken at enqueue time)
    double avg_backlog{0.0};

    /**
     * @return Average encode time in milliseconds
     *
     * @returns Average encode time in milliseconds; 0.0 if no images were written to avoid
     * divide-by-zero
     */
    double avg_encode_ms() const {
      // Avoid divide-by-zero
      if (written == 0) {
        return 0.0;
      }
      return (encode_time_ns_acc / 1e6) / static_cast<double>(written);
    }

    /**
     * @return Average queue wait time in milliseconds
     *
     * @returns Average queue wait time in milliseconds; 0.0 if no images were written to avoid
     * divide-by-zero
     */
    double avg_queue_wait_ms() const {
      if (written == 0) {
        return 0.0;
      }
      return (queue_wait_ns_acc / 1e6) / static_cast<double>(written);
    }

    /**
     * @brief Estimated per-thread encode throughput (fps)
     *
     * @param threads Number of encoding threads
     * @return Estimated per-thread encode throughput in frames per second
     * @note This is approximate; actual parallel overlap may differ.
     */
    double est_per_thread_fps(size_t threads) const {
      if (threads == 0 || encode_time_ns_acc == 0) {
        return 0.0;
      }
      // sum of per-frame times across all threads
      double total_s = encode_time_ns_acc / 1e9;
      double frames = static_cast<double>(written);
      // Each frame's encode time counted once; so average frame time = total_s / frames.
      double avg_frame_s = total_s / frames;
      if (avg_frame_s <= 0) {
        return 0.0;
      }
      double single_thread_capacity = 1.0 / avg_frame_s;
      return single_thread_capacity;
    }
  };

  /**
   * @brief Access image encoding stats
   *
   * @return Image encoding statistics
   */
  ImageEncodeStats image_encode_stats() const {
    ImageEncodeStats s;
    s.enqueued = img_enqueued_.load(std::memory_order_relaxed);
    s.written = img_encoded_.load(std::memory_order_relaxed);
    s.dropped = img_dropped_.load(std::memory_order_relaxed);
    s.encode_time_ns_acc = img_encode_time_ns_acc_.load(std::memory_order_relaxed);
    s.encode_time_ns_max = img_encode_time_ns_max_.load(std::memory_order_relaxed);
    s.queue_wait_ns_acc = img_queue_wait_time_ns_acc_.load(std::memory_order_relaxed);
    s.queue_wait_ns_max = img_queue_wait_time_ns_max_.load(std::memory_order_relaxed);
    s.queue_high_water = img_queue_high_water_.load(std::memory_order_relaxed);
    uint64_t backlog_samples = img_queue_backlog_samples_.load(std::memory_order_relaxed);
    uint64_t backlog_sum = img_queue_backlog_sum_.load(std::memory_order_relaxed);
    if (backlog_samples > 0) {
      s.avg_backlog = static_cast<double>(backlog_sum) / static_cast<double>(backlog_samples);
    }
    return s;
  }

private:
  /**
   * @brief Close writer, output stream, and image workers without finalization.
   *
   * Shared teardown used by both close() and discard_episode(). Does NOT call
   * convert_to_videos() or compute_statistics(). Caller must hold open_mutex_.
   */
  void close_resources();

  /**
   * @brief Write a joint state record to disk
   *
   * @param base Record to write (will be dynamic_cast to JointStateRecord)
   */
  void write_joint_state(const data::RecordBase& base);

  /**
   * @brief Write an image record to disk
   *
   * @param base Record to write (will be dynamic_cast to ImageRecord)
   */
  void write_image(const data::RecordBase& base);

  /**
   * @brief Write images to disk in a worker thread
   */
  void image_worker_loop();

  /**
   * @brief Image encoding job
   */
  struct ImageJob {
    /// @brief Full file path to write
    std::filesystem::path file_path;

    /// @brief Image to write
    cv::Mat image;
  };

  /// @brief Async image encoding members
  std::deque<ImageJob> image_queue_;

  /// @brief Parallel deque storing enqueue steady_clock timestamps for wait time measurement
  std::deque<std::chrono::steady_clock::time_point> image_queue_enqueue_times_;

  /// @brief Mutex protecting image queue
  std::mutex image_queue_mutex_;

  /// @brief Condition variable for image queue
  std::condition_variable image_queue_cv_;

  /// @brief Encoder workers (multi-threaded encoding support)
  std::vector<std::thread> image_workers_;

  /// @brief Config for this backend
  std::shared_ptr<trossen::configuration::LeRobotV2BackendConfig> cfg_;

  /// @brief Metadata for this backend
  ProducerMetadataList metadata_;

  /// @brief Store camera names from metadata for easy access
  std::vector<std::string> camera_names_;

  /// @brief Whether image worker threads should keep running
  std::atomic<bool> image_worker_running_{false};

  /// @brief Basic stats
  std::atomic<uint64_t> img_enqueued_{0};
  std::atomic<uint64_t> img_encoded_{0};
  std::atomic<uint64_t> img_dropped_{0};
  std::atomic<uint64_t> img_encode_time_ns_acc_{0};
  std::atomic<uint64_t> img_encode_time_ns_max_{0};
  std::atomic<uint64_t> img_queue_wait_time_ns_acc_{0};
  std::atomic<uint64_t> img_queue_wait_time_ns_max_{0};
  std::atomic<size_t> img_queue_high_water_{0};
  std::atomic<uint64_t> img_queue_backlog_sum_{0};
  std::atomic<uint64_t> img_queue_backlog_samples_{0};

  /// @brief Derived / cached config values
  size_t max_image_queue_cached_{0};

  std::filesystem::path root_;
  std::filesystem::path images_root_;
  std::filesystem::path videos_root_;
  std::filesystem::path meta_root_;
  std::filesystem::path data_root_;
  std::mutex write_mutex_;
  std::mutex open_mutex_;
  bool opened_{false};

  std::unordered_map<std::string, std::filesystem::path> image_dir_cache_;

  std::shared_ptr<arrow::Schema> schema_;
  std::shared_ptr<arrow::io::FileOutputStream> outfile_;
  std::unique_ptr<parquet::arrow::FileWriter> writer_;

  /// @brief Hash map to store the frame indices for each source
  std::unordered_map<std::string, uint64_t> source_frame_indices_;
};

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2_BACKEND_HPP
