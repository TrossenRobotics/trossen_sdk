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
#include <string>
#include <unordered_map>
#include <vector>

#include <mcap/writer.hpp>

#include "trossen_sdk/io/backend.hpp"
#include "trossen_sdk/io/backends/mcap/mcap_schemas.hpp"

namespace trossen::io::backends {

/**
 * McapBackend writes records into an MCAP file using topic conventions defined in
 * mcap_schemas.hpp. Images are written either as PNG bytes (encoded on the fly) or as raw pixel
 * data accompanied by JSON ImageMeta on a side channel (future).
 */
class McapBackend : public io::Backend {
public:
  struct Config {
    std::string output_path;          // .mcap file path
    std::string robot_name{"/robots/default"}; // prefix used for joint states
    // Chunking / compression options (applied when opening)
    size_t chunk_size_bytes{4 * 1024 * 1024};
    std::string compression; // "" (none) or "zstd" (if library was built with it)
    bool write_image_meta_raw{false}; // future: raw mode
    // Episode context (for session metadata)
    std::string dataset_id;           // Dataset identifier (user-provided or auto-generated UUID)
    uint32_t episode_index{0};        // Episode index within the dataset (zero-based)
  };

  struct Stats {
    uint64_t joint_states_written{0};
    uint64_t images_written{0};
    uint64_t depth_images_written{0};
  };

  explicit McapBackend(Config cfg);
  ~McapBackend() override;

  bool open() override;              // open MCAP writer
  void write(const data::RecordBase& record) override; // dispatch single
  void writeBatch(std::span<const data::RecordBase* const> records) override; // batch
  Stats stats() const { return stats_; }
  void flush() override;             // flush writer
  void close() override;             // finalize file

private:
  struct ChannelInfo {
    mcap::ChannelId id{0};
    std::string topic;
    std::string encoding; // protobuf
    mcap::SchemaId schema_id{0};
  };

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
   * @brief Ensure a session metadata channel exists
   */
  void ensure_sessionmetadata_channel();

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
   * @brief Write session metadata once (on first record write)
   */
  void write_sessionmetadata_once();

  /**
   * @brief Register protobuf schemas once
   */
  void register_schemas_once();

  /// @brief Image schema ID
  mcap::SchemaId schema_image_{0};

  /// @brief Joint state schema ID
  mcap::SchemaId schema_joint_{0};

  /// @brief Session metadata schema ID
  mcap::SchemaId schema_session_{0};

  // TODO(lukeschmitt-tr): camera calibration, robot description support
  /// @brief Camera calibration schema ID
  // mcap::SchemaId schema_cam_calib_{0};

  /// @brief Robot description schema ID
  // mcap::SchemaId schema_robot_description_{0};

  /// @brief MCAP writer instance
  mcap::McapWriter writer_;

  /// @brief MCAP writer options
  mcap::McapWriterOptions opts_{"trossen"};

  /// @brief Output file path
  std::filesystem::path path_;

  bool session_meta_written_{false};
  Config cfg_;
  std::mutex writer_mutex_;
  std::unordered_map<std::string, ChannelInfo> image_channels_; // key: camera name

  // Helpers to classify depth vs color by topic or encoding
  static bool is_depth_topic(const std::string& topic);
  static bool is_depth_encoding(const std::string& enc);
  ChannelInfo joint_channel_{};
  ChannelInfo session_meta_channel_{};
  Stats stats_{};
};

} // namespace trossen::io::backends

#endif // TROSSEN_SDK__IO__BACKENDS__MCAP_BACKEND_HPP
