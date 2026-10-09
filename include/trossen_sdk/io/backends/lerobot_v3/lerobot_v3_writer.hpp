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
   * @brief Result of the parallelizable per-episode preparation stage.
   *
   * Carries everything consume_episode() needs to fold the episode into the
   * dataset: the aligned episode, its channel maps, the resolved task, and one
   * already-encoded per-episode video (plus sampled stat frames) per camera.
   * Holds no writer state, so instances are independent and safe to build on
   * separate threads. `ok` is false when preparation failed (episode skipped).
   */
  struct PreparedEpisode {
    /// @brief One camera's encoded per-episode video + sampled stat frames.
    struct PreparedVideo {
      std::string obs_key;                  ///< LeRobot video key (observation.images.<cam>)
      std::filesystem::path episode_mp4;    ///< encoded per-episode mp4 (consumed by concat)
      double duration_s{0.0};               ///< episode video duration (frame_count / fps)
      std::vector<cv::Mat> samples;         ///< frames sampled for global image stats
      /**
       * @brief Codec of `episode_mp4`, as LeRobot names it in info.json.
       *
       * One of "av1", "h264", "hevc". lerobot derives it from the stream itself, so
       * it must be the canonical codec name and must agree with the actual bitstream.
       * Both the remux and the encode path set it.
       */
      std::string codec{"av1"};
      /// @brief Pixel format of `episode_mp4`: "yuv420p" (color) or "gray12le" (depth).
      std::string pix_fmt{"yuv420p"};
    };
    AlignedEpisode ep;
    McapChannelMap channels;
    std::string task_name;
    std::filesystem::path tmp_dir;          ///< per-episode temp dir; caller removes after consume
    std::vector<PreparedVideo> videos;      ///< first-seen camera order preserved
    bool ok{false};
  };

  /**
   * @brief Create the dataset directory skeleton. Must be called before consume_episode().
   * @return true on success.
   */
  bool open();

  /**
   * @brief Decode + align + encode one episode's video (the parallelizable stage).
   *
   * Loads and aligns the MCAP, extracts camera frames into a per-episode temp
   * dir, encodes each camera's per-episode mp4, samples frames for image stats,
   * and drops the raw JPEGs (keeping only the encoded mp4). Touches no writer
   * state, only Options, so it is safe to call concurrently for different
   * episodes. On any failure it returns a PreparedEpisode with `ok == false`.
   *
   * @param mcap_path Input MCAP file.
   * @param episode_index Zero-based output episode index to stamp.
   * @param fallback_task Task used when the MCAP embeds none.
   * @param tmp_root Root under which the per-episode temp dir is created.
   * @return A PreparedEpisode (check `.ok`).
   */
  PreparedEpisode prepare_episode(
    const std::filesystem::path& mcap_path,
    int episode_index,
    const std::string& fallback_task,
    const std::filesystem::path& tmp_root) const;

  /**
   * @brief Fold one prepared episode into the dataset (the sequential stage).
   *
   * Writes the episode's rows into the current (or freshly rolled) data parquet,
   * concatenates each camera's already-encoded video, accumulates stats,
   * registers the task, and buffers the episode's seek-metadata row. Mutates
   * writer state; call single-threaded, once per episode, in ascending order.
   *
   * @param pe Prepared episode from prepare_episode() (moved-from on success).
   * @return true on success.
   */
  bool consume_episode(PreparedEpisode& pe);

  /**
   * @brief Flush episodes/tasks parquet, global stats.json, info.json, and README.
   * @return true on success.
   */
  bool finalize();

private:
  /// @brief Running state for the shared data parquet stream.
  struct DataFileState {
    int chunk_index{0};
    int file_index{0};
    std::shared_ptr<parquet::arrow::FileWriter> writer;
    std::shared_ptr<arrow::io::FileOutputStream> out;
    std::filesystem::path path;
    int64_t frames_in_file{0};
  };

  bool roll_data_file_if_needed(int64_t next_ep_frames);
  bool open_data_writer(const std::shared_ptr<arrow::Schema>& schema);
  void close_data_writer();
  std::shared_ptr<arrow::Schema> make_data_schema() const;
  std::shared_ptr<arrow::Table> build_episode_table(
    const AlignedEpisode& ep, int episode_index, int task_index, int64_t global_from) const;

  Options opts_;
  std::filesystem::path meta_dir_;
  std::filesystem::path data_dir_;
  std::filesystem::path videos_dir_;

  bool schema_fixed_{false};
  int action_dim_{0};
  int obs_dim_{0};
  std::shared_ptr<arrow::Schema> data_schema_;
  nlohmann::ordered_json features_;  // LeRobot features (built from first episode)

  DataFileState data_;
};

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_V3__LEROBOT_V3_WRITER_HPP_
