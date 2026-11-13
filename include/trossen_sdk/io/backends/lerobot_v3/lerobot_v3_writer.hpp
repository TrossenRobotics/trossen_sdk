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
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <arrow/api.h>
#include <parquet/arrow/writer.h>
#include <opencv2/opencv.hpp>

#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/types/backends/lerobot_v3_backend_config.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_episode.hpp"
#include "trossen_sdk/io/backends/lerobot_v3/lerobot_v3_constants.hpp"

namespace trossen::io::backends {

/**
 * @brief Writes a LeRobot v3.0 dataset by aggregating episodes into shared files.
 *
 * Usage: construct with Options, call open(), then per episode prepare_episode() followed by
 * consume_episode(), then finalize() once. prepare_episode() touches no writer state and may
 * run concurrently on any thread. consume_episode() must be called once per episode, in
 * ascending episode order, from the thread that constructed the writer.
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
   * @brief Result of prepare_episode(): the aligned episode, the resolved task, and one
   *        encoded per-episode video with sampled stat frames per camera.
   *
   * Holds no writer state. `ok` is false when preparation failed (episode skipped).
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
   * @brief Load and align one MCAP and encode each camera's per-episode video.
   *
   * Writes each camera's mp4 in a per-episode temp dir under @p tmp_root and samples frames
   * for image stats; extracted frames are deleted, leaving only the mp4s. Reads only Options,
   * so it is safe to call concurrently for different episodes.
   *
   * @param mcap_path Input MCAP file.
   * @param episode_index Zero-based input index; names the temp dir. consume_episode()
   *        assigns the output index.
   * @param fallback_task Task used when the MCAP embeds none.
   * @param tmp_root Root under which the per-episode temp dir is created.
   * @return A PreparedEpisode; on any failure `ok` is false and its temp dir is removed.
   */
  PreparedEpisode prepare_episode(
    const std::filesystem::path& mcap_path,
    int episode_index,
    const std::string& fallback_task,
    const std::filesystem::path& tmp_root) const;

  /// @brief Outcome of consume_episode().
  enum class ConsumeResult {
    /// @brief The episode was appended to the dataset.
    kAppended,
    /// @brief The episode does not fit the dataset and nothing of it was written, so the
    ///        dataset can take the next episode.
    kRefused,
    /// @brief The writer failed, possibly after part of the episode was written, so the
    ///        dataset cannot take another episode.
    kFailed,
  };

  /**
   * @brief Fold one prepared episode into the dataset (the sequential stage).
   *
   * Writes the episode's rows into the current (or freshly rolled) data parquet,
   * concatenates each camera's already-encoded video, accumulates stats,
   * registers the task, and buffers the episode's seek-metadata row. Mutates
   * writer state; call once per episode, in ascending order, from the thread that
   * constructed the writer.
   *
   * The first appended episode fixes the dataset's row widths, cameras and video codecs.
   * An episode with no rows, or one that differs from the dataset in any of these, is
   * refused before anything of it is stored.
   *
   * @param pe Prepared episode from prepare_episode(); its sampled frames are moved out.
   * @return kAppended on success, kRefused when the episode does not fit the dataset, and
   *         kFailed when called from another thread or on a write failure.
   */
  ConsumeResult consume_episode(PreparedEpisode& pe);

  /**
   * @brief Flush episodes/tasks parquet, global stats.json, info.json, and README.
   * @return true on success.
   */
  bool finalize();

private:
  /// @brief Gives the unit tests access to the private helpers.
  friend class LeRobotV3WriterTestPeer;

  /// @brief File name pattern of extracted depth frames, numbered from 0.
  static constexpr char kDepthFramePattern[] = "image_%06zu.png";

  /// @brief Running state for the shared data parquet stream.
  struct DataFileState {
    /// @brief Chunk index of the open data file.
    int chunk_index{0};
    /// @brief File index of the open data file within its chunk.
    int file_index{0};
    /// @brief Parquet writer of the open data file; null when no file is open.
    std::shared_ptr<parquet::arrow::FileWriter> writer;
    /// @brief Output stream under `writer`; null when no file is open.
    std::shared_ptr<arrow::io::FileOutputStream> out;
    /// @brief Path of the open data file.
    std::filesystem::path path;
    /// @brief Rows written to the open data file.
    int64_t frames_in_file{0};
  };

  /// @brief Running state for one camera's shared video stream.
  struct VideoFileState {
    /// @brief Chunk index of the shared mp4 that receives the next episode.
    int chunk_index{0};
    /// @brief File index of that mp4 within its chunk.
    int file_index{0};
    /// @brief Episode videos staged for that mp4, in order; written by flush_video_file().
    std::vector<std::filesystem::path> parts;
    /// @brief Measured duration of each staged episode video in seconds, one per entry of
    ///        `parts`.
    std::vector<double> part_durations_s;
    /// @brief Bytes of the staged episode videos.
    std::uintmax_t bytes{0};
    /// @brief Measured duration of the staged episode videos in seconds, where the next
    ///        episode starts.
    double duration_s{0.0};
  };

  /// @brief One episode's seek metadata, buffered until finalize().
  struct EpisodeMeta {
    /// @brief Output episode index, the episode's position in the episodes table.
    int episode_index{0};
    /// @brief Tasks performed in the episode.
    std::vector<std::string> tasks;
    /// @brief Number of rows in the episode.
    int64_t length{0};
    /// @brief Chunk index of the data parquet holding the episode's rows.
    int data_chunk_index{0};
    /// @brief File index of the data parquet holding the episode's rows.
    int data_file_index{0};
    /// @brief Dataset-wide index of the episode's first row.
    int64_t dataset_from_index{0};
    /// @brief Dataset-wide index one past the episode's last row.
    int64_t dataset_to_index{0};
    /// @brief Per video key: {chunk_index, file_index, from_timestamp, to_timestamp}, with
    /// the timestamps in seconds from the start of the shared mp4.
    std::map<std::string, std::array<double, 4>> videos;
  };

  /// @brief Make sure an open data file has room for the next episode, opening the next
  ///        chunk/file when the projected uncompressed size reaches the budget.
  /// @param next_ep_frames Rows in the episode about to be written.
  /// @return false when no schema is set or a file fails to close or open.
  bool roll_data_file_if_needed(int64_t next_ep_frames);
  /// @brief Open the data file at the current chunk/file index with data_schema_.
  /// @return false when the file or the parquet writer fails to open.
  bool open_data_writer();
  /// @brief Close the open data file, if any. @return false when closing it fails.
  bool close_data_writer();

  // Video helpers. They run ffmpeg/ffprobe. The color encode, the remux and the frame
  // sampling are shared with the v2 converter (lerobot_common/lerobot_video.hpp).
  /// @brief Encode 16-bit depth PNGs as HEVC gray12le video with 12-bit log quantization.
  bool encode_depth_video(
    const std::filesystem::path& image_dir, size_t frame_count,
    const std::filesystem::path& out_mp4) const;
  /// @brief Encode a file of gray12le depth codes, one frame per row, as lossless HEVC.
  bool encode_depth_codes(
    const std::filesystem::path& raw_path, int width, int height, size_t frame_count,
    const std::filesystem::path& out_mp4) const;
  // Not up for review, changes in #360.
  /**
   * @brief Stage one episode's video for its camera's shared mp4 and report its seek span.
   *
   * The episode's duration is measured with ffprobe, and fails the placement when it
   * differs from `ep_duration_s` by half a frame or more. When the staged videos plus this
   * one reach the size budget, the staged videos are written first and the episode starts
   * the next file.
   *
   * @param video_key Camera's feature key.
   * @param episode_mp4 Episode video; moved into the staging folder.
   * @param ep_duration_s Expected duration, frame count / fps.
   * @param out_slot Set to {chunk_index, file_index, from_timestamp, to_timestamp}.
   * @return false when the episode is not staged; the camera's state is then unchanged.
   */
  bool place_or_concat_video(
    const std::string& video_key, const std::filesystem::path& episode_mp4,
    double ep_duration_s, std::array<double, 4>& out_slot);
  /**
   * @brief Write a camera's staged episode videos into its shared mp4 with one stream copy,
   *        then move the camera to the next file.
   *
   * @return false when writing fails or the written duration differs from the staged
   *         durations by 1 ms or more; the staged videos are then kept.
   */
  bool flush_video_file(const std::string& video_key, VideoFileState& st);
  /// @brief flush_video_file() for every camera. @return false when any camera fails.
  bool flush_videos();

  /**
   * @brief Index of a task in the tasks table, adding it at the next index when new.
   * @param task_name Task string.
   * @return The task's task_index.
   */
  int task_index_for(const std::string& task_name);

  /**
   * @brief Check that an episode fits the dataset, logging what differs when it does not.
   *
   * @param pe Prepared episode.
   * @param action_dim The dataset's action width.
   * @param obs_dim The dataset's observation.state width.
   * @param features The dataset's info.json `features`.
   * @return true when the episode has the same row widths, a video for exactly the
   *         dataset's cameras, and each video in its camera's codec and pixel format.
   */
  bool episode_fits_dataset(const PreparedEpisode& pe, int action_dim, int obs_dim,
                            const nlohmann::ordered_json& features) const;

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
  /// @brief Thread that constructed the writer; the only one allowed to consume episodes.
  std::thread::id owner_thread_{std::this_thread::get_id()};
  /// @brief Metadata directory, `<dataset_root>/meta`; set by open().
  std::filesystem::path meta_dir_;
  /// @brief Data parquet directory, `<dataset_root>/data`; set by open().
  std::filesystem::path data_dir_;
  /// @brief Video directory, `<dataset_root>/videos`; set by open().
  std::filesystem::path videos_dir_;

  /// @brief True once the first episode has fixed the schema and features.
  bool schema_fixed_{false};
  /// @brief The recorded robot_name of the first episode, written as `robot_type`.
  std::string robot_name_;
  /// @brief Width of every action row, fixed by the first episode.
  int action_dim_{0};
  /// @brief Width of every observation.state row, fixed by the first episode.
  int obs_dim_{0};
  /// @brief Schema of every data parquet file, fixed by the first episode.
  std::shared_ptr<arrow::Schema> data_schema_;
  /// @brief info.json `features`, built from the first episode.
  nlohmann::ordered_json features_;

  /// @brief The data parquet file that receives the next episode's rows.
  DataFileState data_;
  /// @brief Each camera's shared video stream, keyed by video_key.
  std::map<std::string, VideoFileState> videos_;
  /// @brief Video keys in the order they were first seen.
  std::vector<std::string> video_keys_;

  /// @brief Seek metadata of every written episode, in episode order.
  std::vector<EpisodeMeta> episodes_;
  /// @brief Task strings indexed by task_index.
  std::vector<std::string> task_list_;
  /// @brief task_index of each task string.
  std::map<std::string, int> task_to_index_;

  /// @brief Rows written so far; the dataset-wide index of the next row.
  int64_t global_frame_index_{0};

  /// @brief Every action value written, one column per dimension, for stats.json.
  std::vector<std::vector<float>> action_values_;
  /// @brief Every observation.state value written, one column per dimension, for stats.json.
  std::vector<std::vector<float>> obs_values_;
  /// @brief Every row timestamp written, in seconds, for stats.json.
  std::vector<float> ts_values_;
  /// @brief Sampled images per video key for image stats, a uniform reservoir capped per key.
  std::map<std::string, std::vector<cv::Mat>> image_samples_;
  /// @brief Sampled images offered per video key so far, kept or not.
  std::map<std::string, size_t> image_samples_seen_;
  /// @brief Fixed-seed generator for the reservoir, so a dataset's stats are reproducible.
  std::mt19937_64 sample_rng_{0};
};

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_V3__LEROBOT_V3_WRITER_HPP_
