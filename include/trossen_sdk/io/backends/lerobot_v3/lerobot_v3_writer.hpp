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

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "trossen_sdk/configuration/types/backends/lerobot_v3_backend_config.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_episode.hpp"
#include "trossen_sdk/io/backends/lerobot_v3/lerobot_v3_constants.hpp"

namespace trossen::io::backends {

/**
 * @brief Writes a LeRobot v3.0 dataset by aggregating episodes into shared files.
 *
 * Conversion is split into a parallelizable per-episode stage and a sequential
 * aggregation stage:
 *   - prepare_episode() decodes + aligns one MCAP, produces each camera's
 *     per-episode video, and samples stat frames. It touches no
 *     writer state (const, works off Options only), so many episodes can be
 *     prepared concurrently on worker threads.
 *   - consume_episode() folds one PreparedEpisode into the shared, size-rolled
 *     data parquet + concatenated video + running stats. It mutates writer
 *     state and must be called once per episode, in ascending episode order,
 *     from the thread that constructed the writer.
 *
 * Usage: construct with Options, call open(), then per episode prepare_episode() (any
 * thread) followed by consume_episode() (constructing thread, in order), then finalize() once.
 */
class LeRobotV3DatasetWriter {
public:
  /// @brief Construction options (mostly from LeRobotV3BackendConfig).
  struct Options {
    /// @brief Full dataset path: <root>/<repository_id>/<dataset_id>.
    std::filesystem::path dataset_root{get_default_root_path() /
                                       configuration::LEROBOT_V3_DEFAULT_REPOSITORY_ID /
                                       auto_generate_dataset_id()};
    /// @brief License identifier written to info.json and the README.
    std::string license{configuration::LEROBOT_V3_DEFAULT_LICENSE};
    /// @brief Dataset frame rate (Hz).
    float fps{configuration::LEROBOT_V3_DEFAULT_FPS};
    /// @brief Maximum files per chunk directory before rolling to the next chunk index.
    int chunks_size{lerobot_v3::DEFAULT_CHUNK_SIZE};
    /// @brief Roll to a new data parquet file once it reaches this size (megabytes, 10^6 bytes).
    int data_files_size_in_mb{lerobot_v3::DEFAULT_DATA_FILE_SIZE_IN_MB};
    /// @brief Roll to a new video file once it reaches this size (megabytes, 10^6 bytes).
    int video_files_size_in_mb{lerobot_v3::DEFAULT_VIDEO_FILE_SIZE_IN_MB};
    /// @brief Write camera videos; false writes the data parquet and metadata only.
    bool encode_videos{configuration::LEROBOT_V3_DEFAULT_ENCODE_VIDEOS};
    /// @brief Threads per video encoder, passed as SVT-AV1 `lp=` (color) and x265
    /// `pools=` (depth); 0 sets no cap.
    int encoder_threads{0};
  };

  /**
   * @brief Store the options; nothing is written until open().
   * @param opts Dataset location, frame rate, roll sizes and video settings.
   */
  explicit LeRobotV3DatasetWriter(Options opts);

  /// @brief Close the open data parquet file, if any.
  ~LeRobotV3DatasetWriter();

  LeRobotV3DatasetWriter(const LeRobotV3DatasetWriter&) = delete;
  LeRobotV3DatasetWriter& operator=(const LeRobotV3DatasetWriter&) = delete;

  /**
   * @brief Result of the parallelizable per-episode preparation stage.
   *
   * Carries everything consume_episode() needs to fold the episode into the
   * dataset: the aligned episode, the resolved task, and one
   * already-encoded per-episode video (plus sampled stat frames) per camera.
   * Holds no writer state, so instances are independent and safe to build on
   * separate threads. `ok` is false when preparation failed (episode skipped).
   */
  struct PreparedEpisode {
    /// @brief One camera's encoded per-episode video + sampled stat frames.
    struct PreparedVideo {
      std::string obs_key;                  ///< video key (observation.images.<cam>)
      std::filesystem::path episode_mp4;    ///< encoded per-episode mp4 (consumed by concat)
      double duration_s{0.0};               ///< episode video duration (frame_count / fps)
      std::vector<cv::Mat> samples;         ///< frames sampled for global image stats
      /// @brief Codec of `episode_mp4` as written to info.json: "av1", "h264" or "hevc".
      /// Matches the bitstream.
      std::string codec{"av1"};
      /// @brief Pixel format of `episode_mp4`: "yuv420p" (color) or "gray12le" (depth).
      std::string pix_fmt{"yuv420p"};
    };
    /// @brief Decoded episode with every stream aligned onto the dataset's rows.
    AlignedEpisode ep;
    /// @brief Task of the episode: the one embedded in the MCAP, else the fallback task.
    std::string task_name;
    std::filesystem::path tmp_dir;          ///< per-episode temp dir; caller removes after consume
    std::vector<PreparedVideo> videos;      ///< first-seen camera order preserved
    /// @brief True when preparation succeeded and the episode can be consumed.
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
   * Loads and aligns the MCAP, produces each camera's per-episode mp4 in a
   * per-episode temp dir, samples frames for image stats, and deletes any
   * extracted frames (keeping only the encoded mp4). Touches no writer
   * state, only Options, so it is safe to call concurrently for different
   * episodes. On any failure it returns a PreparedEpisode with `ok == false`.
   *
   * @param mcap_path Input MCAP file.
   * @param episode_index Zero-based input index; names the temp dir. consume_episode()
   *        assigns the output index.
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
   * writer state; call once per episode, in ascending order, from the thread that
   * constructed the writer.
   *
   * @param pe Prepared episode from prepare_episode(); its sampled frames are moved out.
   * @return true on success.
   */
  bool consume_episode(PreparedEpisode& pe);

  /**
   * @brief Flush episodes/tasks parquet, global stats.json, info.json, and README.
   * @return true on success.
   */
  bool finalize();

private:
  /// @brief Close the open data file, if any. @return false when closing it fails.
  bool close_data_writer();

  // Video helpers. They run ffmpeg/ffprobe. The color encode, the remux and the frame
  // sampling are shared with the v2 converter (lerobot_common/lerobot_video.hpp).
  /// @brief Encode 16-bit depth PNGs as HEVC gray12le video with 12-bit log quantization.
  bool encode_depth_video(
    const std::filesystem::path& image_dir, size_t frame_count,
    const std::filesystem::path& out_mp4) const;
  // Not up for review, changes in #360.
  bool place_or_concat_video(
    const std::string& video_key, const std::filesystem::path& episode_mp4,
    double ep_duration_s, std::array<double, 4>& out_slot);

  /**
   * @brief Index of a task in the tasks table, adding it at the next index when new.
   * @param task_name Task string.
   * @return The task's task_index.
   */
  int task_index_for(const std::string& task_name);

  /// @brief Write the episodes metadata parquet, one row of seek metadata per episode.
  /// @return false when the file cannot be written.
  bool write_episodes_parquet();
  /// @brief Write TASKS_PATH, one row per task in task_index order.
  /// @return false when the file cannot be written.
  bool write_tasks_parquet();
  /// @brief Write STATS_PATH with the statistics of every feature over the whole dataset.
  /// @return false when the file cannot be written.
  bool write_stats_json();
  /// @brief Write INFO_PATH with the dataset totals, features and path templates.
  /// @return false when the file cannot be written.
  bool write_info_json();
  /// @brief Write the dataset README.
  /// @return false when the file cannot be written.
  bool write_readme();

  /// @brief Options given at construction.
  Options opts_;
  /// @brief Metadata directory, `<dataset_root>/meta`; set by open().
  std::filesystem::path meta_dir_;
  /// @brief Data parquet directory, `<dataset_root>/data`; set by open().
  std::filesystem::path data_dir_;
  /// @brief Video directory, `<dataset_root>/videos`; set by open().
  std::filesystem::path videos_dir_;
};

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_V3__LEROBOT_V3_WRITER_HPP_
