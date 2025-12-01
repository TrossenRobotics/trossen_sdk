/**
 * @file null_backend.hpp
 * @brief Backend that discards all records (for benchmarking enqueue/drain overhead).
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__NULL_BACKEND_HPP
#define TROSSEN_SDK__IO__BACKENDS__NULL_BACKEND_HPP

#include <atomic>

#include "trossen_sdk/io/backend.hpp"

namespace trossen::io::backends {

/**
 * @brief Backend that discards all records.
 *
 * Useful for benchmarking enqueue/drain overhead without storage latency.
 */
class NullBackend : public io::Backend {
public:
  /**
   * @brief Construct a NullBackend with the given URI
   */
  explicit NullBackend(const std::string& uri = "null://") : Backend(uri) {}

  /**
   * @brief Open a null logging destination
   *
   * @param uri Voided backend uri
   * @return true on success
   */
  bool open() override {
    opened_ = true;
    return true;
  }

  /**
   * @brief Discard a single record, counting it.
   *
   * @param record Voided record
   */
  void write(const data::RecordBase& record) override {
    (void)record;
    ++count_;
  }

  /**
   * @brief Discard a batch of records, counting them.
   *
   * @param records Voided record pointers
   */
  void writeBatch(std::span<const data::RecordBase* const> records) override {
    count_ += records.size();
  }

  /**
   * @brief No-op flush
   */
  void flush() override {}

  /**
   * @brief Close the null backend
   */
  void close() override {
    opened_ = false;
  }

  /**
   * @brief Get the number of records "written"
   *
   * @return Count of records
   */
  uint64_t count() const {
    return count_.load();
  }

private:
  /// @brief Count of records "written"
  std::atomic<uint64_t> count_{0};

  /// @brief Whether the backend is opened
  bool opened_{false};
};

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__NULL_BACKEND_HPP
