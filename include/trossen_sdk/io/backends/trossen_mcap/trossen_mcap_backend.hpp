/**
 * @file trossen_mcap_backend.hpp
 * @brief TrossenMCAP backend: writes records to a TrossenMCAP file.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP_BACKEND_HPP
#define TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP_BACKEND_HPP

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "foxglove/channel.hpp"
#include "foxglove/foxglove.hpp"
#include "foxglove/mcap.hpp"
#include "foxglove/schemas.hpp"
#include "trossen_sdk/configuration/types/backends/trossen_mcap_backend_config.hpp"
#include "trossen_sdk/io/backend.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"
#include "trossen_sdk/utils/depth_quantization.hpp"
#include "trossen_sdk/utils/video_encoder.hpp"

namespace trossen::io::backends {

/// @brief Initial buffer size for encoded messages
const size_t TROSSEN_MCAP_INITIAL_ENCODED_BUFFER_SIZE = 1024 * 1024;  // 1 MB

/**
 * @brief Generate a UUIDv7 episode id (RFC 9562) in canonical form
 * @return A lowercase canonical UUID string, e.g. "0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f"
 *
 * UUIDv7 leads with a 48-bit Unix millisecond timestamp, so the canonical string is
 * lexicographically time-ordered: sorting filenames by name matches recording order.
 * That keeps ordering stable across distributed machines merged into one dataset (the id
 * is globally unique without coordination) and lets consumers that sort by filename (e.g.
 * the cloud episode listing) present episodes chronologically. The remaining 74 bits come
 * from std::random_device, so the id is also a unique identifier for the episode.
 */
inline std::string generate_episode_id() {
  const auto now = std::chrono::system_clock::now();
  const std::uint64_t unix_ts_ms = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());

  std::random_device rd;
  std::uniform_int_distribution<std::uint64_t> dist;
  const std::uint64_t rand_a = dist(rd);  // 12 bits used
  const std::uint64_t rand_b = dist(rd);  // 62 bits used

  // Assemble the 128 bits into two 64-bit halves per RFC 9562.
  const std::uint64_t high =
    (unix_ts_ms << 16)              // 48-bit ms timestamp in bits 63..16
    | (std::uint64_t{0x7} << 12)    // version 7 in bits 15..12
    | (rand_a & 0x0FFFULL);         // 12 random bits in bits 11..0
  const std::uint64_t low =
    (std::uint64_t{0x2} << 62)      // variant 0b10 in the top 2 bits
    | (rand_b & 0x3FFFFFFFFFFFFFFFULL);  // 62 random bits

  std::ostringstream oss;
  oss << std::hex << std::setfill('0')
      << std::setw(8) << (high >> 32) << '-'
      << std::setw(4) << ((high >> 16) & 0xFFFFULL) << '-'
      << std::setw(4) << (high & 0xFFFFULL) << '-'
      << std::setw(4) << (low >> 48) << '-'
      << std::setw(12) << (low & 0xFFFFFFFFFFFFULL);
  return oss.str();
}

/**
 * @brief TrossenMCAPBackend writes records into a TrossenMCAP file.
 */
class TrossenMCAPBackend : public io::Backend {
public:
  /**
   * @brief Statistics about written records
   */
  struct Stats {
    /// @brief Number of joint state records written
    uint64_t joint_states_written{0};

    /// @brief Number of 2D odometry records written
    uint64_t odometry_2d_written{0};

    /// @brief Number of image records written
    uint64_t images_written{0};

    /// @brief Number of depth images written
    uint64_t depth_images_written{0};

    /// @brief Video frames dropped because a stream's encode queue was full
    uint64_t video_frames_dropped{0};
  };

  /**
   * @brief Construct a TrossenMCAPBackend with the given configuration
   *
   * @param metadata Optional producer metadata
   */
  explicit TrossenMCAPBackend(
    const ProducerMetadataList& metadata = {});

  /**
   * @brief Destructor
   */
  ~TrossenMCAPBackend() override;

  /**
   * @brief Prepare backend for a new episode
   */
  void preprocess_episode() override;

  /**
   * @brief Open the MCAP writer
   *
   * @return true on success, false otherwise
   */
  bool open() override;

  /**
   * @brief Serialize and persist a single record
   *
   * @param record Record to write
   */
  void write(const data::RecordBase& record) override;

  /**
   * @brief Serialize and persist a batch of records
   *
   * @param records Span of record pointers (non-owning); lifetime must cover call
   */
  void write_batch(std::span<const data::RecordBase* const> records) override;

  /**
   * @brief Flush any buffered data
   */
  void flush() override;

  /**
   * @brief Close the backend
   */
  void close() override;

  /**
   * @brief Discard episode data and delete the MCAP file
   */
  void discard_episode() override;

  /**
   * @brief Get statistics about written records
   *
   * @return Stats structure with counts
   */
  Stats stats() const { return stats_; }

  /**
   * @brief Count existing episode files in the dataset directory
   *
   * @return Number of existing episode files (0 if none)
   */
  uint32_t scan_existing_episodes() override;

  /**
   * @brief Path of the .mcap file for the current episode
   *
   * @return The output path as a string (empty if open() has not run)
   */
  std::string current_output_path() const override { return path_.string(); }

private:
  /**
   * @brief Close all channels and writer without deleting files.
   *
   * Shared teardown used by both close() and discard_episode().
   * Caller must hold writer_mutex_.
   */
  void close_resources();

  /**
   * @brief Find the most-recently-written <uuid>.mcap episode file in the dataset dir
   *
   * Used by discard_episode() when this backend never opened a file (the re-record
   * path creates a fresh backend), so there is no stored path_ to delete.
   *
   * @return Path to the newest matching episode file, or an empty path if none found
   */
  std::filesystem::path find_latest_episode_file() const;

  /**
   * @brief Ensure an image channel exists for the given camera name
   *
   * @param camera_name Name of the camera (used as stream ID)
   * @return Pointer to the channel, or nullptr on failure
   */
  foxglove::RawChannel* ensure_image_channel(const std::string& camera_name);

  /**
   * @brief Ensure an image channel exists for the given camera name, with additional metadata
   *
   * @param camera_name Name of the camera (used as stream ID)
   * @param metadata Key/value pairs to add to the MCAP Channel metadata map
   * @return Pointer to the channel, or nullptr on failure
   */
  foxglove::RawChannel* ensure_image_channel_with_metadata(
    const std::string& camera_name,
    const std::unordered_map<std::string, std::string>& metadata);

  /**
   * @brief Ensure the joint state channel exists for a given stream ID
   *
   * @param stream_id Stream identifier (e.g., "leader_left", "follower_right")
   * @return Pointer to the channel, or nullptr on failure
   */
  foxglove::RawChannel* ensure_jointstate_channel(const std::string& stream_id);

  /**
   * @brief Ensure the 2D odometry channel exists for a given stream ID
   *
   * @param stream_id Stream identifier (e.g., "base")
   * @return Pointer to the channel, or nullptr on failure
   */
  foxglove::RawChannel* ensure_odometry_2d_channel(const std::string& stream_id);

  /**
   * @brief Ensure the per-frame metadata channel exists for a camera stream
   *
   * @param stream_id Camera stream identifier (e.g., "camera_high", "camera_high_depth")
   * @return Pointer to the channel, or nullptr on failure
   */
  foxglove::RawChannel* ensure_camera_meta_channel(const std::string& stream_id);

  /**
   * @brief Write one frame's timing to the camera's metadata channel
   *
   * The image topics use Foxglove schemas, which carry a single timestamp and cannot
   * hold a device capture time. This writes the full clock set for the same frame on a
   * parallel topic. Call it only after the frame itself was written: each message carries
   * the frame's position in this episode's stream, which readers join on.
   *
   * @param stream_id Camera stream identifier, matching the image topic
   * @param ts Timestamps for the frame
   * @param seq Producer sequence number of the frame
   * @param device_frame_number The camera's own frame counter, when it reports one
   * @param log_time_ns MCAP log time, the same one the frame was logged at
   * @param repeat True when the frame repeats the previous one to fill a grid slot
   */
  void write_camera_meta_record(const std::string& stream_id, const data::Timestamp& ts,
                                uint64_t seq, std::optional<uint64_t> device_frame_number,
                                uint64_t log_time_ns, bool repeat = false);

  /**
   * @brief Write an image record
   *
   * @param img Image record to write
   */
  void write_image_record(const data::ImageRecord& img);

  /// @brief One frame waiting to be encoded. The record is a copy, but its cv::Mat shares the
  /// producer's pixel buffer, so queuing a frame does not copy its pixels.
  struct VideoJob {
    data::ImageRecord img;
    bool depth{false};
    foxglove::RawChannel* channel{nullptr};
    /// Grid slot the frame was captured in, counted from the episode's grid origin; may be
    /// negative for a frame captured before it. Unset when the backend has no grid.
    std::optional<int64_t> slot;
  };

  /**
   * @brief One camera stream's encoder and the thread that runs it.
   *
   * A hardware encode round trip takes longer than a camera frame period, so streams sharing one
   * thread cannot keep up with several cameras. Each stream therefore encodes on its own thread,
   * outside writer_mutex_, which it takes only to log the finished packet. The queue is bounded:
   * when encoding falls behind, new frames are dropped and counted instead of accumulating in
   * memory without limit.
   */
  struct VideoStream {
    std::unique_ptr<utils::VideoEncoder> encoder;
    std::mutex mutex;              ///< Guards queue, stopping, drain.
    std::condition_variable cv;    ///< Signalled on a new job or a stop request.
    std::deque<VideoJob> queue;    ///< Frames waiting to be encoded, oldest first.
    bool stopping{false};          ///< Set by stop_video_streams().
    bool drain{true};              ///< On stop: encode what is queued (true) or abandon it.
    bool failure_reported{false};  ///< An encode failure on this stream was already logged.
    uint64_t dropped{0};           ///< Frames dropped on a full queue; guarded by writer_mutex_.
    std::vector<uint16_t> depth_quant_lut;  ///< Built on the first depth frame.
    std::thread thread;

    // Grid placement. Fixed at creation, then touched only by this stream's thread; the
    // counters are read after the thread is joined.
    uint64_t grid_origin_ns{0};        ///< Realtime of slot 0, shared by every stream.
    double grid_period_ns{0.0};        ///< Slot length; 0 when the backend has no grid.
    std::optional<int64_t> next_slot;  ///< First slot not yet written.
    std::optional<data::ImageRecord> last_frame;  ///< Last frame written; repeated into gaps.
    uint64_t repeated{0};    ///< Slots filled by repeating the previous frame.
    uint64_t superseded{0};  ///< Frames dropped because their slot was already written.
  };

  /**
   * @brief Queue one frame on its stream's encoder thread, creating the stream on first use.
   *
   * Caller must hold writer_mutex_.
   *
   * @param img Image record to encode
   * @param depth True for a lossless H.265 depth stream, false for H.264 color
   * @param channel Channel the encoded frame is logged to
   */
  void submit_video_frame(const data::ImageRecord& img, bool depth, foxglove::RawChannel* channel);

  /**
   * @brief Body of a stream's encoder thread: encode queued frames until stopped.
   *
   * @param stream The stream this thread serves
   */
  void video_stream_loop(VideoStream& stream);

  /**
   * @brief Convert and encode one frame. Runs on the stream's thread without writer_mutex_.
   *
   * @param stream The stream the frame belongs to
   * @param img Image record to encode
   * @param depth True for a depth frame, false for color
   * @return The encoded frame; empty data if the frame could not be encoded
   */
  utils::VideoEncoder::EncodedFrame encode_video_frame(VideoStream& stream,
                                                       const data::ImageRecord& img, bool depth);

  /**
   * @brief Log one encoded frame as foxglove.CompressedVideo. Caller must hold writer_mutex_.
   *
   * @param img Image record the packet was encoded from
   * @param depth True for a depth frame, false for color
   * @param packet Encoded frame
   * @param codec Codec the packet was encoded with
   * @param channel Channel to log the message to
   * @param time_ns Message timestamp and MCAP log time: the capture time, or the slot time
   *        when the stream is on a grid
   * @return true if the frame was logged; false if it was dropped
   */
  bool log_video_frame(const data::ImageRecord& img, bool depth,
                       const utils::VideoEncoder::EncodedFrame& packet, utils::VideoCodec codec,
                       foxglove::RawChannel* channel, uint64_t time_ns);

  /**
   * @brief Encode one frame and log it with its metadata. Runs on the stream's thread.
   *
   * @param stream The stream the frame belongs to
   * @param job The frame, its depth flag and channel
   * @param time_ns Log time for the packet and its metadata
   * @param repeat True when the frame repeats the previous one into a missed grid slot
   */
  void encode_and_log(VideoStream& stream, const VideoJob& job, uint64_t time_ns, bool repeat);

  /**
   * @brief Time a frame was captured, on the realtime clock: the exposure time when the
   *        camera's clock is mapped onto realtime, else the host delivery time.
   */
  static uint64_t capture_time_ns(const data::Timestamp& ts);

  /**
   * @brief Stop and join every stream's encoder thread.
   *
   * Caller must NOT hold writer_mutex_: a draining thread needs it to log what it encodes.
   *
   * @param drain true to encode and log every queued frame first, false to abandon them
   */
  void stop_video_streams(bool drain);

  /**
   * @brief Serialize one cv::Mat as foxglove.RawImage and log it
   *
   * @param image Pixel data to serialize
   * @param frame_id Frame id to stamp on the message
   * @param width Image width in pixels
   * @param height Image height in pixels
   * @param encoding Pixel encoding string (e.g. "bgr8")
   * @param ts Capture timestamp
   * @param channel Channel to log the message to
   * @param counter Stats counter bumped on success
   * @return true if the frame was logged; false if it was dropped
   */
  bool write_raw_image_message(const cv::Mat& image, const std::string& frame_id, uint32_t width,
                               uint32_t height, const std::string& encoding,
                               const data::Timespec& ts, foxglove::RawChannel* channel,
                               uint64_t* counter);

  /**
   * @brief Write one frame as video or raw image, depending on `records_video()`
   *
   * A raw frame and its frame metadata are logged immediately. A video frame is queued on its
   * stream's encoder thread, which logs both once it is encoded.
   *
   * @param img Image record to write
   * @param depth True for a depth frame, false for color
   * @param channel Channel to log the message to
   */
  void write_image_frame(const data::ImageRecord& img, bool depth, foxglove::RawChannel* channel);

  /**
   * @brief Write a joint state record
   *
   * @param js Joint state record to write
   */
  void write_jointstate_record(const data::JointStateRecord& js);

  /**
   * @brief Write a 2D odometry record
   *
   * @param odom 2D odometry record to write
   */
  void write_odometry_2d_record(const data::Odometry2DRecord& odom);

  /**
   * @brief Register protobuf schemas once
   */
  void register_schemas_once();

  /// @brief Foxglove context
  foxglove::Context context_;

  /// @brief Foxglove MCAP writer instance
  std::optional<foxglove::McapWriter> writer_;

  /// @brief Serialised FileDescriptorSet for the JointState protobuf schema
  std::string schema_data_js_;

  /// @brief Serialised FileDescriptorSet for the Odometry2D protobuf schema
  std::string schema_data_odom2d_;

  /// @brief Serialised FileDescriptorSet for the FrameMeta protobuf schema
  std::string schema_data_frame_meta_;

  /// @brief Output file path
  std::filesystem::path path_;

  /// @brief Configuration options
  std::shared_ptr<trossen::configuration::TrossenMCAPBackendConfig> cfg_;

  /// @brief Mutex to protect writer access
  std::mutex writer_mutex_;

  /// @brief Map of image channels by camera name
  std::unordered_map<std::string, foxglove::RawChannel> image_channels_;

  /// @brief Per-camera frame metadata channels, keyed by stream id
  std::unordered_map<std::string, foxglove::RawChannel> camera_meta_channels_;

  /// @brief Frames written so far this episode, per camera stream; the next frame's index
  std::unordered_map<std::string, uint64_t> camera_meta_frame_index_;

  /// @brief Per-stream video encoders and their threads, keyed by ImageRecord::id.
  /// Guarded by writer_mutex_; every entry is stopped and joined before the episode closes.
  std::unordered_map<std::string, std::unique_ptr<VideoStream>> video_streams_;

  /// @brief Streams whose encoder could not be created; their frames are dropped
  std::unordered_map<std::string, bool> video_encode_failed_;

  /// @brief Capture time of the episode's first video frame from any camera, which is slot 0
  /// of the shared grid. Guarded by writer_mutex_; cleared when the episode closes.
  std::optional<uint64_t> grid_origin_ns_;

  /// @brief Helper to identify depth topics
  static bool is_depth_topic(const std::string& topic);

  /// @brief Helper to identify depth encodings
  static bool is_depth_encoding(const std::string& enc);

  /// @brief Map of joint state channels by stream ID
  std::unordered_map<std::string, foxglove::RawChannel> joint_channels_;

  /// @brief Map of 2D odometry channels by stream ID
  std::unordered_map<std::string, foxglove::RawChannel> odometry_2d_channels_;

  /// @brief Statistics about written records
  Stats stats_{};

  /// @brief Cached producer metadata for writing dataset info to MCAP on open
  ProducerMetadataList producer_metadata_;
};

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP_BACKEND_HPP
