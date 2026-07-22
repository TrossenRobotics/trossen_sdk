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
 * mcap_dataset_loader, so it can sit behind any future v3 producer.
 */

#ifndef TROSSEN_SDK__SCRIPTS__COMMON__LEROBOT_V3_WRITER_HPP_
#define TROSSEN_SDK__SCRIPTS__COMMON__LEROBOT_V3_WRITER_HPP_

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

#include "mcap_dataset_loader.hpp"

namespace trossen::convert {

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
   * state — only Options — so it is safe to call concurrently for different
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

  /// @brief Running state for one camera's shared video stream.
  struct VideoFileState {
    int chunk_index{0};
    int file_index{0};
    std::filesystem::path path;       // current shared mp4 (empty until first episode)
    double duration_s{0.0};           // running end timestamp within the current file
  };

  /// @brief One episode's seek metadata, buffered until finalize().
  struct EpisodeMeta {
    int episode_index{0};
    std::vector<std::string> tasks;
    int64_t length{0};
    int data_chunk_index{0};
    int data_file_index{0};
    int64_t dataset_from_index{0};
    int64_t dataset_to_index{0};
    // Per video key: {chunk_index, file_index, from_timestamp, to_timestamp}.
    std::map<std::string, std::array<double, 4>> videos;
  };

  bool roll_data_file_if_needed(int64_t next_ep_frames);
  bool open_data_writer(const std::shared_ptr<arrow::Schema>& schema);
  void close_data_writer();
  std::shared_ptr<arrow::Schema> make_data_schema() const;
  std::shared_ptr<arrow::Table> build_episode_table(
    const AlignedEpisode& ep, int task_index, int64_t global_from) const;

  // Video helpers (defined in the .cpp; shell out to ffmpeg/ffprobe).
  bool encode_episode_video(
    const std::filesystem::path& image_dir, size_t frame_count,
    const std::filesystem::path& out_mp4) const;
  bool place_or_concat_video(
    const std::string& video_key, const std::filesystem::path& episode_mp4,
    double ep_duration_s, std::array<double, 4>& out_slot);

  int task_index_for(const std::string& task_name);

  bool write_episodes_parquet();
  bool write_tasks_parquet();
  bool write_stats_json();
  bool write_info_json();
  bool write_readme();

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
  std::map<std::string, VideoFileState> videos_;  // keyed by video_key
  std::vector<std::string> video_keys_;            // stable order, first-seen

  std::vector<EpisodeMeta> episodes_;
  std::vector<std::string> task_list_;             // task_index → task string
  std::map<std::string, int> task_to_index_;

  int64_t total_frames_{0};
  int64_t global_frame_index_{0};

  // Global stat accumulators (computed at finalize → meta/stats.json).
  std::vector<std::vector<float>> action_values_;       // per-dim flattened
  std::vector<std::vector<float>> obs_values_;          // per-dim flattened
  std::vector<float> ts_values_;
  // Sampled images per video key for image stats (capped).
  std::map<std::string, std::vector<cv::Mat>> image_samples_;
};

}  // namespace trossen::convert

#endif  // TROSSEN_SDK__SCRIPTS__COMMON__LEROBOT_V3_WRITER_HPP_
