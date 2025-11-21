/**
 * @file producer_base.hpp
 * @brief Base classes for hardware producers (polled and push).
 */

#ifndef TROSSEN_SDK__HW__PRODUCER_BASE_HPP
#define TROSSEN_SDK__HW__PRODUCER_BASE_HPP

#include <functional>
#include <memory>
#include <atomic>
#include <cstdint>
#include <string>

#include "trossen_sdk/data/record.hpp"

namespace trossen::hw {

/// @brief Statistics for producers
struct ProducerStats {

  /// @brief Total records emitted
  uint64_t produced{0};

  /// @brief Total records dropped
  uint64_t dropped{0};

  /// @brief Records intentionally discarded during warmup (not counted as dropped)
  uint64_t warmup_discarded{0};
};

/**
 * @brief PolledProducer: scheduler calls poll() periodically; implementation may emit 0 or more records.
 */
class PolledProducer {
public:
  /**
   * @brief Virtual destructor
   */
  virtual ~PolledProducer() = default;

  /**
   * @brief Poll the producer for new data and emit records via the callback
   *
   * @param emit Callback to invoke for each produced record
   */
  virtual void poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) = 0;

  /**
   * @brief Get producer statistics
   *
   * @return const reference to ProducerStats
   */
  const ProducerStats& stats() const {return stats_; };

  /// @brief Metadata for producers
  struct ProducerMetadata {
    /// @brief Unique identifier for the producer
    std::string id;

    /// @brief Human-readable name for the producer
    std::string name;

    /// @brief Description of the producer's function
    std::string description;
  };

  /**
   * @brief Get producer metadata
   *
   * @return const reference to ProducerMetadata
   */
  virtual const PolledProducer::ProducerMetadata& metadata() const {return metadata_; };

protected:
  /// @brief Whether we've opened the device
  bool opened_{false};

  /// @brief Last capture monotonic timestamp for inter-frame delta
  uint64_t last_capture_mono_{0};

  /// @brief Accumulated inter-frame delta nanoseconds
  uint64_t if_accum_ns_{0};

  /// @brief Max inter-frame delta nanoseconds
  uint64_t if_max_ns_{0};

  /// @brief Sample count for inter-frame delta
  uint64_t if_samples_{0};

  /// @brief Next frame count threshold for FPS health log
  uint64_t next_health_report_frame_{300};

  /// @brief Internal statistics
  ProducerStats stats_{};

  /// @brief Monotonic sequence number for emitted records
  uint64_t seq_{0};

  /// @brief Producer metadata
  PolledProducer::ProducerMetadata metadata_{};
};

// PushProducer: owns its own internal thread(s); start registers emit callback.
class PushProducer {
public:
  /**
   * @brief Virtual destructor
   */
  virtual ~PushProducer() = default;

  /**
   * @brief Start the producer's internal thread(s) and register the emit callback
   *
   * @param emit Callback to invoke for each produced record
   * @return true on success
   */
  virtual bool start(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) = 0;

  /**
   * @brief Stop the producer's internal thread(s)
   */
  virtual void stop() = 0;

  /**
   * @brief Get producer statistics
   *
   * @return const reference to ProducerStats
   */
  const ProducerStats& stats() const {return stats_; };
protected:
  /// @brief Whether we've opened the device
  bool opened_{false};

  /// @brief Last capture monotonic timestamp for inter-frame delta
  uint64_t last_capture_mono_{0};

  /// @brief Accumulated inter-frame delta nanoseconds
  uint64_t if_accum_ns_{0};

  /// @brief Max inter-frame delta nanoseconds
  uint64_t if_max_ns_{0};

  /// @brief Sample count for inter-frame delta
  uint64_t if_samples_{0};

  /// @brief Next frame count threshold for FPS health log
  uint64_t next_health_report_frame_{300};

  /// @brief Internal statistics
  ProducerStats stats_{};

  /// @brief Monotonic sequence number for emitted records
  uint64_t seq_{0};

};

} // namespace trossen::hw

#endif  // TROSSEN_SDK__HW__PRODUCER_BASE_HPP
