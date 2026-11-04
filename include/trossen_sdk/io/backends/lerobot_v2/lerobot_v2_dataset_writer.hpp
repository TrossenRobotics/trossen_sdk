/**
 * @file lerobot_v2_dataset_writer.hpp
 * @brief Writes MCAP recordings into a LeRobot v2.1 dataset.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2__LEROBOT_V2_DATASET_WRITER_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2__LEROBOT_V2_DATASET_WRITER_HPP_

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

#include "nlohmann/json.hpp"

#include "trossen_sdk/io/backends/lerobot_common/lerobot_episode.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp"

namespace trossen::io::backends {

/**
 * @brief Writes MCAP recordings into a LeRobot v2.1 dataset.
 *
 * Usage: construct with Options, then per recording prepare_episode() followed by
 * consume_episode(), then finalize() once.
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
    /// @brief Episodes per chunk folder.
    int chunk_size{1000};
    /// @brief Task for an episode whose recording embeds none.
    std::string task_name;
    /// @brief Cores each AV1 encode may use (SVT-AV1 lp=).
    int encoder_threads{0};
  };

  /// @brief One recording converted into a scratch folder, not yet part of the dataset.
  struct PreparedEpisode {
    /// @brief The input MCAP file.
    std::filesystem::path mcap_file;
    /// @brief The aligned rows and camera info from load_aligned_episode().
    AlignedEpisode ep;
    /// @brief The recording's channel maps from load_aligned_episode().
    McapChannelMap channels;
    /// @brief Extracted frames of each camera, under the scratch folder.
    std::map<std::string, std::filesystem::path> image_dirs;
    /// @brief Episode video of each camera, under the scratch folder.
    std::map<std::string, std::filesystem::path> videos;
    /// @brief Codec and pixel format of each remuxed camera, which keeps the recorder's codec.
    std::map<std::string, VideoCodecInfo> remuxed_codecs;
  };

  /**
   * @brief Store the options; nothing is written until a write call.
   * @param opts Dataset location, license, chunk size, default task and encoder threads.
   */
  explicit LeRobotV2DatasetWriter(Options opts);

  /// @brief The dataset folder: root / repository_id / dataset_id.
  std::filesystem::path dataset_path() const;

  /**
   * @brief Decode one recording and build its images and videos under tmp_dir.
   *
   * Every check that can reject a recording runs here, and nothing in the dataset is
   * written, so a rejected recording leaves the dataset as it was. It changes no writer
   * state, so several recordings can be prepared at once on different threads.
   *
   * @param mcap_file Path to the input MCAP file.
   * @param input_index Position of the recording in the input, used for logging only.
   * @param tmp_dir Scratch folder for this recording; the caller removes it.
   * @param out Filled with the episode on success.
   * @return true on success.
   */
  bool prepare_episode(const std::filesystem::path& mcap_file, int input_index,
                       const std::filesystem::path& tmp_dir, PreparedEpisode& out) const;

  /**
   * @brief Write a prepared episode into the dataset as the next episode index.
   *
   * Writes the parquet, moves the images and videos to their names for that index and
   * updates the metadata files. Indices start at 0 and advance only when an episode is
   * written. A failure after the parquet is written removes it, so the episode leaves no
   * rows behind.
   *
   * Call it from one thread, in the order the episodes are to be numbered.
   *
   * @param pe Episode from prepare_episode().
   * @return true on success.
   */
  bool consume_episode(PreparedEpisode& pe);

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
  /// @brief Dataset-wide index of the next row written.
  int64_t global_index_offset_{0};
  /// @brief Index of the next episode written.
  int next_episode_index_{0};
};

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2__LEROBOT_V2_DATASET_WRITER_HPP_
