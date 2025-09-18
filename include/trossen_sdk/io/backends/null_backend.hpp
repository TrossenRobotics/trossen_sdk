/**
 * @file null_backend.hpp
 * @brief Backend that discards all records (for benchmarking enqueue/drain overhead).
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__NULL_BACKEND_HPP
#define TROSSEN_SDK__IO__BACKENDS__NULL_BACKEND_HPP

#include "trossen_sdk/io/backend.hpp"
#include <atomic>

namespace trossen::io::backends {

/**
 * @brief Backend that discards all records.
 *
 * Useful for benchmarking enqueue/drain overhead without storage latency.
 */
class NullBackend : public io::Backend {
public:
  /**
   * @brief Open a null logging destination
   *
   * @param uri Voided backend uri
   * @return true on success
   */
  bool open(const std::string& uri) override {
    (void)uri;
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

  void flush() override {}
  void close() override {
    opened_ = false;
  }
  uint64_t count() const {
    return count_.load();
  }
private:
  std::atomic<uint64_t> count_{0};
  bool opened_{false};
};

} // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__NULL_BACKEND_HPP
