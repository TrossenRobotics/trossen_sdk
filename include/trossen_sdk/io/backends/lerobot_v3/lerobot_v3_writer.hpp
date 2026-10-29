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
#include <string>
#include <vector>

#include <arrow/api.h>
#include <parquet/arrow/writer.h>
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
 * Usage: construct with Options, call open(), prepare_episode() (any thread) →
 * consume_episode() (constructing thread, in order) per episode, then finalize() once.
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
    AlignedEpisode ep;
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
  /// @brief Gives the unit tests access to the private helpers.
  friend class LeRobotV3WriterTestPeer;

  /// @brief File name pattern of extracted color frames, numbered from 0.
  static constexpr char kColorFramePattern[] = "image_%06d.jpg";
  /// @brief File name pattern of extracted depth frames, numbered from 0.
  static constexpr char kDepthFramePattern[] = "image_%06zu.png";

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
    /// @brief Chunk index of the shared mp4 that receives the next episode.
    int chunk_index{0};
    /// @brief File index of that mp4 within its chunk.
    int file_index{0};
    /// @brief Episode videos staged for that mp4, in order; written by flush_video_file().
    std::vector<std::filesystem::path> parts;
    /// @brief Duration of the staged episode videos in seconds, where the next episode
    ///        starts.
    double duration_s{0.0};
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
  /// @brief The data parquet columns, with list widths from action_dim_ and obs_dim_.
  std::shared_ptr<arrow::Schema> make_data_schema() const;
  /// @brief One episode's rows. @return nullptr when a column fails to build.
  std::shared_ptr<arrow::Table> build_episode_table(
    const AlignedEpisode& ep, int episode_index, int task_index, int64_t global_from) const;

  // Video helpers. They run ffmpeg/ffprobe.
  bool encode_episode_video(
    const std::filesystem::path& image_dir, size_t frame_count,
    const std::filesystem::path& out_mp4) const;
  /// @brief Encode 16-bit depth PNGs as HEVC gray12le video with 12-bit log quantization.
  bool encode_depth_video(
    const std::filesystem::path& image_dir, size_t frame_count,
    const std::filesystem::path& out_mp4) const;
  /**
   * @brief Wrap an already-compressed Annex B stream in mp4 without re-encoding.
   *
   * The bitstream is copied through verbatim (`-c copy`), so the recorded frames
   * reach LeRobot exactly as captured. Timestamps are generated at the dataset
   * fps because an elementary stream carries none.
   *
   * @param annexb Input elementary stream (`.h264` / `.hevc`).
   * @param frame_count Expected frame count, checked against the muxed result.
   * @param out_mp4 Output mp4.
   * @return true on success.
   */
  bool remux_episode_video(
    const std::filesystem::path& annexb, size_t frame_count,
    const std::filesystem::path& out_mp4) const;

  /**
   * @brief Decode evenly spaced frames from an mp4 for global image statistics.
   *
   * Used on the remux path, where no decoded frames pass through the converter
   * at all. Every frame is decoded and an evenly spaced sample is kept.
   *
   * @param mp4 Video to sample.
   * @param frame_count Total frames in the video.
   * @param tmp_dir Scratch directory for the extracted stills.
   * @return Sampled frames (empty on failure; stats degrade rather than fail).
   */
  std::vector<cv::Mat> sample_video_frames(
    const std::filesystem::path& mp4, size_t frame_count,
    const std::filesystem::path& tmp_dir) const;

  // Not up for review, changes in #360.
  /**
   * @brief Stage one episode's video for its camera's shared mp4 and report its seek span.
   *
   * @param video_key Camera's feature key.
   * @param episode_mp4 Episode video; moved into the staging folder.
   * @param ep_duration_s Episode duration in seconds, frame count / fps.
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
   * @return false when writing fails; the staged videos are then kept.
   */
  bool flush_video_file(const std::string& video_key, VideoFileState& st);
  /// @brief flush_video_file() for every camera. @return false when any camera fails.
  bool flush_videos();

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

  int action_dim_{0};
  int obs_dim_{0};
  std::shared_ptr<arrow::Schema> data_schema_;

  DataFileState data_;
  /// @brief Each camera's shared video stream, keyed by video_key.
  std::map<std::string, VideoFileState> videos_;
};

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_V3__LEROBOT_V3_WRITER_HPP_
