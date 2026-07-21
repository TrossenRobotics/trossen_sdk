/**
 * @file trossen_mcap_backend.hpp
 * @brief TrossenMCAP backend: writes records to a TrossenMCAP file.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP_BACKEND_HPP
#define TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP_BACKEND_HPP

#include <chrono>
#include <cstdint>
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
#include <unordered_map>
#include <vector>

#include "foxglove/channel.hpp"
#include "foxglove/foxglove.hpp"
#include "foxglove/mcap.hpp"
#include "foxglove/schemas.hpp"

#include "trossen_sdk/io/backend.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"
#include "trossen_sdk/configuration/types/backends/trossen_mcap_backend_config.hpp"

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
   * @brief Write an image record
   *
   * @param img Image record to write
   */
  void write_image_record(const data::ImageRecord& img);

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

  /// @brief Output file path
  std::filesystem::path path_;

  /// @brief Configuration options
  std::shared_ptr<trossen::configuration::TrossenMCAPBackendConfig> cfg_;

  /// @brief Mutex to protect writer access
  std::mutex writer_mutex_;

  /// @brief Map of image channels by camera name
  std::unordered_map<std::string, foxglove::RawChannel> image_channels_;

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
