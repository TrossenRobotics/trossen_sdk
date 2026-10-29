/**
 * @file lerobot_v3_constants.hpp
 * @brief Constants for the LeRobot v3.0 dataset layout.
 *
 * A v3.0 dataset stores many episodes in shared data and video files, grouped into
 * chunk-XXX/file-YYY and rolled to a new file by size. Per-episode metadata is
 * parquet.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_V3__LEROBOT_V3_CONSTANTS_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_V3__LEROBOT_V3_CONSTANTS_HPP_

namespace trossen::io::backends::lerobot_v3 {

/// @brief Codebase version written to info.json.
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

// ── info.json path templates, stored verbatim ──

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
 * @brief Advance (chunk_index, file_index) to the next file.
 *
 * The file index wraps to 0 in the next chunk once it reaches chunks_size, so the
 * n-th file is chunk = n / chunks_size, file = n % chunks_size.
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
