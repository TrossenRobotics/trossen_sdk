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
#include <string>
#include <span>

#include "trossen_sdk/data/record.hpp"

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
  Backend(const std::string& uri) : uri_(uri) {};

  /**
   * @brief Virtual destructor
   */
  virtual ~Backend() = default;

  /**
   * @brief Open a logging destination
   * @param uri Path or logical destination identifier
   * @return true on success
   */
  virtual bool open() = 0;

  /**
   * @brief Serialize & persist a single record
   * @param record Record to write
   *
   * Default implementation may forward to writeBatch for uniform handling
   */
  virtual void write(const data::RecordBase& record) = 0;

  /**
   * @brief Serialize & persist a contiguous batch of records
   * @param records Span of record pointers (non-owning); lifetime must cover call
   *
   * Implementations may override for more efficient encoding (e.g. shared compression
   * window). Default implementation will loop and call write(record) if not overridden.
   */
  virtual void writeBatch(std::span<const data::RecordBase* const> records) {
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

protected:
  /// Destination identifier (file path, etc.)
  std::string uri_{""};
};

/** @brief Unique owning pointer for a backend implementation. */
using BackendPtr = std::unique_ptr<Backend>;

} // namespace trossen::io

#endif // TROSSEN__IO__BACKEND_HPP
