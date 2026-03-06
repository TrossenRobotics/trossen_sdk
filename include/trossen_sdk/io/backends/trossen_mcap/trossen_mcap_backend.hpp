/**
 * @file trossen_mcap_backend.hpp
 * @brief TrossenMCAP backend: writes records to a TrossenMCAP file.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP_BACKEND_HPP
#define TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP_BACKEND_HPP

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <span>
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
   *
   * @param output_path Output file path for this episode
   * @param episode_index Zero-based episode index (unused)
   * @param dataset_id Dataset identifier (unused)
   * @param repository_id Repository identifier (unused)
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
   * @brief Get statistics about written records
   *
   * @return Stats structure with counts
   */
  Stats stats() const { return stats_; }

  /**
   * @brief Scan directory for existing episode files and return next index
   *
   * @return Next episode index (max_found + 1, or 0 if none found)
   */
  uint32_t scan_existing_episodes() override;

private:
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
};

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__TROSSEN_MCAP_BACKEND_HPP
