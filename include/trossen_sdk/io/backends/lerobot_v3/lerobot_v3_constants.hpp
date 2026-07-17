/**
 * @file lerobot_v3_constants.hpp
 * @brief Constants for the LeRobot v3.0 dataset layout.
 *
 * v3.0 differs from v2.1 chiefly by *aggregating* many episodes into shared
 * data/video files (rolled by size), storing per-episode metadata as parquet
 * (not jsonl), and carrying seek indices that let a reader locate each episode's
 * rows and video segment within the shared files. These constants capture the
 * v3.0 path templates and default file-size thresholds, mirroring
 * huggingface/lerobot (CODEBASE_VERSION "v3.0").
 *
 * Kept in a dedicated `lerobot_v3` namespace to avoid clashing with the v2.1
 * constants in trossen::io::backends.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_V3__LEROBOT_V3_CONSTANTS_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_V3__LEROBOT_V3_CONSTANTS_HPP_

namespace trossen::io::backends::lerobot_v3 {

/// @brief LeRobot codebase version written to info.json.
inline constexpr char CODEBASE_VERSION[] = "v3.0";

// ── Directory names ──

/// @brief Metadata directory.
inline constexpr char META_DIR[] = "meta";
/// @brief Data (parquet) directory.
inline constexpr char DATA_DIR[] = "data";
/// @brief Video directory.
inline constexpr char VIDEO_DIR[] = "videos";

// ── Metadata filenames (relative to the dataset root) ──

/// @brief Info JSON path.
inline constexpr char INFO_PATH[] = "meta/info.json";
/// @brief Global aggregate statistics path.
inline constexpr char STATS_PATH[] = "meta/stats.json";
/// @brief Tasks parquet path (columns: task_index, task).
inline constexpr char TASKS_PATH[] = "meta/tasks.parquet";

// ── Chunk/file path templates (used at runtime via std::format-style fills) ──
// chunk and file indices are zero-padded to 3 digits, e.g. chunk-000/file-000.

/// @brief Data parquet path template: chunk_index, file_index.
inline constexpr char DATA_PATH_FMT[] = "data/chunk-{:03d}/file-{:03d}.parquet";
/// @brief Video path template: video_key, chunk_index, file_index.
inline constexpr char VIDEO_PATH_FMT[] = "videos/{}/chunk-{:03d}/file-{:03d}.mp4";
/// @brief Episode-metadata parquet path template: chunk_index, file_index.
inline constexpr char EPISODES_PATH_FMT[] = "meta/episodes/chunk-{:03d}/file-{:03d}.parquet";

// ── info.json path templates (Python-style format strings, stored verbatim) ──
// These are the literal strings LeRobot writes into info.json and resolves at
// read time; they must match the on-disk layout produced above.

/// @brief data_path field for info.json.
inline constexpr char INFO_DATA_PATH[] =
  "data/chunk-{chunk_index:03d}/file-{file_index:03d}.parquet";
/// @brief video_path field for info.json.
inline constexpr char INFO_VIDEO_PATH[] =
  "videos/{video_key}/chunk-{chunk_index:03d}/file-{file_index:03d}.mp4";

// ── Default aggregation thresholds (overridable via config) ──

/// @brief Maximum files per chunk before rolling to the next chunk index.
inline constexpr int DEFAULT_CHUNK_SIZE = 1000;
/// @brief Roll to a new data parquet file once its projected size reaches this (MB).
inline constexpr int DEFAULT_DATA_FILE_SIZE_IN_MB = 100;
/// @brief Roll to a new video file once its projected size reaches this (MB).
inline constexpr int DEFAULT_VIDEO_FILE_SIZE_IN_MB = 200;

/**
 * @brief Advance (chunk_index, file_index) the LeRobot way: file index wraps to the
 *        next chunk once it reaches chunks_size.
 *
 * Mirrors lerobot's update_chunk_file_indices: the global file number maps to
 * chunk = number / chunks_size, file = number % chunks_size.
 *
 * @param chunk_index In/out chunk index.
 * @param file_index In/out file index.
 * @param chunks_size Maximum files per chunk.
 */
inline void update_chunk_file_indices(int& chunk_index, int& file_index, int chunks_size) {
  if (file_index >= chunks_size - 1) {
    file_index = 0;
    ++chunk_index;
  } else {
    ++file_index;
  }
}

}  // namespace trossen::io::backends::lerobot_v3

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_V3__LEROBOT_V3_CONSTANTS_HPP_
