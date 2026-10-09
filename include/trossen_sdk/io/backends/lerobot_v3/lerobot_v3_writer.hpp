/**
 * @file lerobot_v3_writer.hpp
 * @brief Stateful writer that aggregates aligned episodes into a LeRobot v3.0 dataset.
 *
 * Unlike the v2.1 layout (one parquet + one mp4 per episode), v3.0 concatenates
 * many episodes into shared, size-rolled data parquet and video files, and stores
 * per-episode seek metadata as parquet. This writer is fed one AlignedEpisode at a
 * time (offline, all episodes available), keeps an open data ParquetWriter across
 * episodes, concatenates per-camera video, and on finalize() emits the episodes /
 * tasks parquet, global stats.json, info.json, and README.
 *
 * It depends only on the format-agnostic AlignedEpisode produced by
 * mcap_dataset_loader.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_V3__LEROBOT_V3_WRITER_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_V3__LEROBOT_V3_WRITER_HPP_

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <parquet/arrow/writer.h>
#include <opencv2/opencv.hpp>

#include "nlohmann/json.hpp"

#include "trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp"

namespace trossen::io::backends {

/**
 * @brief Writes a LeRobot v3.0 dataset by aggregating episodes into shared files.
 *
 * Conversion is split into a parallelizable per-episode stage and a sequential
 * aggregation stage:
 *   - prepare_episode() decodes + aligns one MCAP, extracts camera frames,
 *     encodes the per-episode video, and samples stat frames. It touches no
 *     writer state (const, works off Options only), so many episodes can be
 *     prepared concurrently on worker threads.
 *   - consume_episode() folds one PreparedEpisode into the shared, size-rolled
 *     data parquet + concatenated video + running stats. It mutates writer
 *     state and MUST be called single-threaded, once per episode, in ascending
 *     episode order.
 *
 * Usage: construct with Options, call open(), prepare_episode() (any thread) →
 * consume_episode() (main thread, in order) per episode, then finalize() once.
 */
class LeRobotV3DatasetWriter {
public:
  /// @brief Construction options (mostly from LeRobotV3BackendConfig).
  struct Options {
    /// @brief Full dataset path: <root>/<repository_id>/<dataset_id>.
    std::filesystem::path dataset_root;
    std::string robot_name{"trossen"};
    std::string license{"apache-2.0"};
    float fps{30.0f};
    int chunks_size{1000};
    int data_files_size_in_mb{100};
    int video_files_size_in_mb{200};
    bool encode_videos{true};
    /// @brief Emit the native lerobot_trossen schema (joint/camera naming + gray12le depth).
    bool native_schema{false};
    /**
     * @brief Decode and re-encode video-backed recordings to AV1 instead of remuxing.
     *
     * Recordings that already store compressed video are stream-copied by
     * default: the frames were encoded once at capture time, and h264/hevc are
     * both first-class LeRobot codecs. Set this to reproduce the AV1 output
     * byte-format of datasets converted before in-MCAP video existed. No effect
     * on recordings that store raw images, which always encode.
     */
    bool reencode_av1{false};
    /**
     * @brief Per-encoder parallelism cap; 0 leaves each encoder to size itself.
     *
     * Applied as SVT-AV1 `lp=` (color) and x265 `pools=` (depth) rather than
     * ffmpeg's generic `-threads`, which libsvtav1 ignores outright and libx265
     * only partly honours. Needed because several episodes encode concurrently:
     * without a cap every encoder sizes itself to the whole machine, so N
     * workers oversubscribe it N-fold.
     */
    int encoder_threads{0};
  };

  explicit LeRobotV3DatasetWriter(Options opts);
  ~LeRobotV3DatasetWriter();

  LeRobotV3DatasetWriter(const LeRobotV3DatasetWriter&) = delete;
  LeRobotV3DatasetWriter& operator=(const LeRobotV3DatasetWriter&) = delete;

  /**
   * @brief Create the dataset directory skeleton. Must be called before consume_episode().
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
  std::filesystem::path meta_dir_;
  std::filesystem::path data_dir_;
  std::filesystem::path videos_dir_;
};

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_V3__LEROBOT_V3_WRITER_HPP_
