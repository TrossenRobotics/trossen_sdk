/**
 * @file lerobot_table.hpp
 * @brief The data parquet rows shared by the LeRobot v2.1 and v3.0 formats.
 *
 * Both formats store an episode's rows with the same seven columns: action,
 * observation.state, timestamp, frame_index, episode_index, index and task_index.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_TABLE_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_TABLE_HPP_

#include <cstdint>
#include <filesystem>
#include <memory>

#include <arrow/api.h>

#include "trossen_sdk/io/backends/lerobot_common/lerobot_episode.hpp"

namespace trossen::io::backends {

/**
 * @brief The data parquet columns.
 * @param action_dim Values in each action row.
 * @param obs_dim Values in each observation.state row.
 */
std::shared_ptr<arrow::Schema> make_data_schema(int action_dim, int obs_dim);

/**
 * @brief One episode's rows, with the schema of make_data_schema(action_dim, obs_dim).
 *
 * frame_index counts from 0 within the episode and index from global_from across the
 * dataset.
 *
 * @return nullptr when a row has another width or a column fails to build.
 */
std::shared_ptr<arrow::Table> build_episode_table(
  const AlignedEpisode& ep, int action_dim, int obs_dim, int episode_index, int task_index,
  int64_t global_from);

/**
 * @brief Write a table to one Snappy-compressed parquet file, as a single row group.
 *
 * @param table Table to write.
 * @param path Output file; removed again when writing or closing it fails. A file that
 *   could not be opened is left untouched.
 * @param store_schema Embed the Arrow schema and its metadata in the file.
 * @return true on success.
 */
bool write_parquet_file(
  const arrow::Table& table, const std::filesystem::path& path, bool store_schema);

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_TABLE_HPP_
