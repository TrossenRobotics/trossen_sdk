/**
 * @file lerobot_v2_dataset_writer.hpp
 * @brief Writes MCAP recordings into a LeRobot v2.1 dataset.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2__LEROBOT_V2_DATASET_WRITER_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2__LEROBOT_V2_DATASET_WRITER_HPP_

#include <filesystem>
#include <string>

#include "nlohmann/json.hpp"

namespace trossen::io::backends {

/**
 * @brief Writes MCAP recordings into a LeRobot v2.1 dataset.
 *
 * Usage: construct with Options, write the episodes, then call finalize() once.
 */
class LeRobotV2DatasetWriter {
public:
  /// @brief Where the dataset is written.
  struct Options {
    /// @brief Root folder of all datasets.
    std::filesystem::path root;
    /// @brief Repository folder under root.
    std::string repository_id;
    /// @brief Dataset folder under repository_id.
    std::string dataset_id;
    /// @brief License identifier written to the README.
    std::string license;
  };

  /**
   * @brief Store the options; nothing is written until a write call.
   * @param opts Dataset location and license.
   */
  explicit LeRobotV2DatasetWriter(Options opts);

  /// @brief The dataset folder: root / repository_id / dataset_id.
  std::filesystem::path dataset_path() const;

  /**
   * @brief Write the per-episode stats of every episode in the dataset, then the README.
   *
   * Rewrites meta/episodes_stats.jsonl with one line per episode parquet under data/. A
   * dataset without meta/info.json gets no stats. Each failure is a warning on stderr and
   * leaves the episodes as they are.
   */
  void finalize() const;

private:
  /**
   * @brief Stats of one episode's parquet columns and camera images.
   * @param parquet_path The episode's parquet file.
   * @param episode_index The episode's index, which names its image folders.
   * @param info The dataset's info.json, whose video features name the cameras.
   * @return The stats object of one episodes_stats.jsonl line.
   * @throws std::runtime_error if the parquet cannot be read.
   */
  nlohmann::ordered_json compute_episode_stats(const std::filesystem::path& parquet_path,
                                               int episode_index,
                                               const nlohmann::json& info) const;

  /// @brief Options from the constructor.
  Options opts_;
};

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2__LEROBOT_V2_DATASET_WRITER_HPP_
