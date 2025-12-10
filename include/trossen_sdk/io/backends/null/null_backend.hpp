/**
 * @file null_backend.hpp
 * @brief Backend that discards all records (for benchmarking enqueue/drain overhead).
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__NULL_BACKEND_HPP
#define TROSSEN_SDK__IO__BACKENDS__NULL_BACKEND_HPP

#include <atomic>
#include <memory>
#include <vector>

#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/io/backend.hpp"
#include "trossen_sdk/configuration/types/backends/null_backend_config.hpp"


namespace trossen::io::backends {

/**
 * @brief Backend that discards all records.
 *
 * Useful for benchmarking enqueue/drain overhead without storage latency.
 */
class NullBackend : public io::Backend {
public:
  /**
   * @brief Configuration for NullBackend
   */
  struct Config : public io::Backend::Config {
    /// @brief URI for the null backend (defaults to "null://")
    std::string uri{"null://"};
  };

  /**
   * @brief Construct a NullBackend with the given configuration
   *
   * @param cfg Configuration options
   * @param metadata Optional producer metadata
   */
  explicit NullBackend(
    const Config& cfg,
    const ProducerMetadataList& metadata = {});

  /**
   * @brief Open a null logging destination
   *
   * @return true on success
   */
  bool open() override;

  /**
   * @brief Discard a single record, counting it.
   *
   * @param record Voided record
   */
  void write(const data::RecordBase& record) override;

  /**
   * @brief Discard a batch of records, counting them.
   *
   * @param records Voided record pointers
   */
  void write_batch(std::span<const data::RecordBase* const> records) override;

  /**
   * @brief No-op flush
   */
  void flush() override;

  /**
   * @brief Close the null backend
   */
  void close() override;

  /**
   * @brief Get the number of records "written"
   *
   * @return Count of records
   */
  uint64_t count() const;

  /**
   * @brief Scan directory for existing episode files and return next index
   */
  uint32_t scan_existing_episodes() override {
    // NullBackend has no files, so always return 0
    return 0;
  };

private:
  /// @brief Count of records "written"
  std::atomic<uint64_t> count_{0};

  /// @brief Whether the backend is opened
  bool opened_{false};
};

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__NULL_BACKEND_HPP
