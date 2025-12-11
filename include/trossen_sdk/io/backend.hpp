/**
 * @file backend.hpp
 * @brief Backend interface for persisting records.
 *
 * A backend encapsulates the serialization and durability strategy (e.g. writing MCAP, JSON lines,
 * proprietary binary, etc.). Sinks forward batches of records to an implementation of this
 * interface.
 */

#ifndef TROSSEN_SDK__IO__BACKEND_HPP
#define TROSSEN_SDK__IO__BACKEND_HPP

#include <memory>
#include <span>
#include <string>

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/types.hpp"

namespace trossen::io {

/**
 * @brief Abstract sink backend responsible for storage.
 */
class Backend {
public:
  /**
   * @brief Construct a backend with the given destination URI
   *
   * @param uri Path or logical destination identifier
   */
  explicit Backend(){}

  /**
   * @brief Virtual destructor
   */
  virtual ~Backend() = default;

  /**
   * @brief Configuration struct for backend.
   */
  struct Config {
    /// @brief Base config options for all backends can be added here
    std::string type{""};

    /**
     * @brief Virtual destructor
     */
    virtual ~Config() = default;
  };

  /**
   * @brief Prepare backend for a new episode with runtime parameters
   *
   * @param output_path Base output path for this episode
   * @param episode_index Zero-based episode index
   * @param dataset_id Dataset identifier
   * @param repository_id Repository identifier (may be empty)
   *
   * This method is called after backend construction but before open() to allow backends to
   * configure episode-specific paths and metadata. Default implementation does nothing (backends
   * that don't need per-episode customization can ignore this).
   */
  virtual void preprocess_episode() {}

  /**
   * @brief Open a logging destination
   *
   * @return true on success, false otherwise
   * @note Must be called before data can be written
   */
  virtual bool open() = 0;

  /**
   * @brief Serialize & persist a single record
   *
   * @param record Record to write
   *
   * Default implementation may forward to write_batch for uniform handling
   */
  virtual void write(const data::RecordBase& record) = 0;

  /**
   * @brief Serialize & persist a contiguous batch of records
   *
   * @param records Span of record pointers (non-owning); lifetime must cover call
   *
   * Implementations may override for more efficient encoding (e.g. shared compression window).
   * Default implementation will loop and call write(record) if not overridden.
   */
  virtual void write_batch(std::span<const data::RecordBase* const> records) {
    for (auto* r : records) {
      if (r) write(*r);
    }
  }

  /**
   * @brief Flush any buffered data
   */
  virtual void flush() = 0;

  /**
   * @brief Close the backend
   */
  virtual void close() = 0;

  /**
   * @brief Scan directory for existing episode files and return next index
   * @return Next episode index to use
   */
  virtual uint32_t scan_existing_episodes() = 0;

  /**
   * @brief Set episode index
   *
   * Default implementation sets the episode_index_ member.
   * Children can override if custom behavior is needed.
   */
  virtual void set_episode_index(uint32_t episode_index) {
    episode_index_ = episode_index;
  }

protected:
  /// @brief Whether the backend is opened
  bool opened_{false};

  /// @brief Episode index for current recording session
  uint32_t episode_index_{0};
};

}  // namespace trossen::io

#endif  // TROSSEN__IO__BACKEND_HPP
