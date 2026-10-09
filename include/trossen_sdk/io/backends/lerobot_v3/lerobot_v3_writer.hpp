/**
 * @file lerobot_v3_writer.hpp
 * @brief Stateful writer that aggregates aligned episodes into a LeRobot v3.0 dataset.
 *
 * The writer takes one AlignedEpisode at a time and appends it to shared, size-rolled
 * data parquet and per-camera video files. finalize() writes the episodes and tasks
 * parquet, stats.json, info.json and the README.
 *
 * It depends only on the format-agnostic AlignedEpisode produced by
 * mcap_dataset_loader.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_V3__LEROBOT_V3_WRITER_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_V3__LEROBOT_V3_WRITER_HPP_

#include <filesystem>
#include <string>

#include "trossen_sdk/configuration/types/backends/lerobot_v3_backend_config.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_v3/lerobot_v3_constants.hpp"

namespace trossen::io::backends {

/**
 * @brief Writes a LeRobot v3.0 dataset by aggregating episodes into shared files.
 *
 * Usage: construct with Options, call open(), then finalize() once.
 */
class LeRobotV3DatasetWriter {
public:
  /// @brief Construction options (mostly from LeRobotV3BackendConfig).
  struct Options {
    /// @brief Full dataset path: <root>/<repository_id>/<dataset_id>.
    std::filesystem::path dataset_root{get_default_root_path() /
                                       configuration::LEROBOT_V3_DEFAULT_REPOSITORY_ID /
                                       auto_generate_dataset_id()};
    std::string robot_name{DEFAULT_ROBOT_NAME};
    std::string license{configuration::LEROBOT_V3_DEFAULT_LICENSE};
    float fps{configuration::LEROBOT_V3_DEFAULT_FPS};
    int chunks_size{lerobot_v3::DEFAULT_CHUNK_SIZE};
    int data_files_size_in_mb{lerobot_v3::DEFAULT_DATA_FILE_SIZE_IN_MB};
    int video_files_size_in_mb{lerobot_v3::DEFAULT_VIDEO_FILE_SIZE_IN_MB};
    bool encode_videos{configuration::LEROBOT_V3_DEFAULT_ENCODE_VIDEOS};
    /// @brief Threads per video encoder, passed as SVT-AV1 `lp=` (color) and x265
    /// `pools=` (depth); 0 sets no cap.
    int encoder_threads{0};
  };

  explicit LeRobotV3DatasetWriter(Options opts);
  ~LeRobotV3DatasetWriter();

  LeRobotV3DatasetWriter(const LeRobotV3DatasetWriter&) = delete;
  LeRobotV3DatasetWriter& operator=(const LeRobotV3DatasetWriter&) = delete;

  /**
   * @brief Create the dataset directory skeleton. Must be called before the first episode.
   * @return true on success.
   */
  bool open();

  /**
   * @brief Flush episodes/tasks parquet, global stats.json, info.json, and README.
   * @return true on success.
   */
  bool finalize();

private:
  void close_data_writer();

  Options opts_;
};

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_V3__LEROBOT_V3_WRITER_HPP_
