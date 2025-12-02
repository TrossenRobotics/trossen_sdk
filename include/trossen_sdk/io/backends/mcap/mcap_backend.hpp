/**
 * @file mcap_backend.hpp
 * @brief MCAP backend: writes records to an MCAP file.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__MCAP_BACKEND_HPP
#define TROSSEN_SDK__IO__BACKENDS__MCAP_BACKEND_HPP

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
#include "trossen_sdk/io/backends/mcap/mcap_schemas.hpp"

namespace trossen::io::backends {

/// @brief Initial buffer size for encoded messages
const size_t MCAP_INITIAL_ENCODED_BUFFER_SIZE = 1024 * 1024;  // 1 MB

/**
 * @brief McapBackend writes records into an MCAP file.
 */
class McapBackend : public io::Backend {
public:
  /**
   * @brief Configuration options for McapBackend
   */
  struct Config : public io::Backend::Config {
    /// @brief .mcap file path
    std::string output_path;

    /// @brief prefix used for joint states
    std::string robot_name{"/robot/joint_states"};

    /// @brief Chunking / compression options (applied when opening)
    size_t chunk_size_bytes{4 * 1024 * 1024};

    /// @brief "" (none) or "zstd" (if library was built with it)
    std::string compression{""};

    /// @brief Dataset identifier (user-provided or auto-generated UUID)
    std::string dataset_id;

    /// @brief Episode index within the dataset (zero-based)
    uint32_t episode_index{0};
  };

  /**
   * @brief Statistics about written records
   */
  struct Stats {
    /// @brief Number of joint state records written
    uint64_t joint_states_written{0};

    /// @brief Number of image records written
    uint64_t images_written{0};

    /// @brief Number of depth images written
    uint64_t depth_images_written{0};
  };

  /**
   * @brief Construct an McapBackend with the given configuration
   *
   * @param cfg Configuration options
   */
  explicit McapBackend(Config cfg);

  /**
   * @brief Destructor
   */
  ~McapBackend() override;

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
  void writeBatch(std::span<const data::RecordBase* const> records) override;

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
   * @param base_path Directory to scan
   * @return Next episode index (max_found + 1, or 0 if none found)
   */
  static uint32_t scan_existing_episodes(const std::filesystem::path& base_path);

private:
  /**
   * @brief Ensure an image channel exists for the given camera name
   *
   * @param camera_name Name of the camera (used as stream ID)
   */
  void ensure_image_channel(const std::string& camera_name);

  /**
   * @brief Ensure an image channel exists for the given camera name, with additional metadata
   *
   * @param camera_name Name of the camera (used as stream ID)
   * @param metadata Key/value pairs to add to the MCAP Channel metadata map
   */
  void ensure_image_channel_with_metadata(
    const std::string& camera_name,
    const std::unordered_map<std::string, std::string>& metadata);

  /**
   * @brief Ensure the joint state channel exists
   */
  void ensure_jointstate_channel();

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
   * @brief Register protobuf schemas once
   */
  void register_schemas_once();

  /// @brief Foxglove context
  foxglove::Context context_;

  /// @brief Foxglove MCAP writer instance
  std::optional<foxglove::McapWriter> writer_;

  /// @brief Protobuf schema for images
  std::string schema_data_;

  /// @brief Output file path
  std::filesystem::path path_;

  /// @brief Configuration options
  Config cfg_;

  /// @brief Mutex to protect writer access
  std::mutex writer_mutex_;

  /// @brief Map of image channels by camera name
  std::unordered_map<std::string, foxglove::RawChannel> image_channels_;

  /// @brief Helper to identify depth topics
  static bool is_depth_topic(const std::string& topic);

  /// @brief Helper to identify depth encodings
  static bool is_depth_encoding(const std::string& enc);

  /// @brief Joint state channel
  std::optional<foxglove::RawChannel> joint_channel_;

  /// @brief Statistics about written records
  Stats stats_{};
};

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__MCAP_BACKEND_HPP
