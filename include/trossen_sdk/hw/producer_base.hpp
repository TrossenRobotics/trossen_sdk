/**
 * @file producer_base.hpp
 * @brief Base classes for hardware producers.
 */

#ifndef TROSSEN_SDK__HW__PRODUCER_BASE_HPP
#define TROSSEN_SDK__HW__PRODUCER_BASE_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

#include "nlohmann/json.hpp"

#include "trossen_sdk/data/record.hpp"

namespace trossen::hw {

// Forward declaration: producers store and expose their backing component as a
// shared_ptr but never dereference it here, so an incomplete type suffices.
class HardwareComponent;

/**
 * @brief Statistics for a producer
 */
struct ProducerStats {
  /// @brief Total records emitted
  uint64_t produced{0};

  /// @brief Total records dropped
  uint64_t dropped{0};

  /// @brief Records intentionally discarded during warmup (not counted as dropped)
  uint64_t warmup_discarded{0};
};

/**
 * @brief PolledProducer: scheduler calls poll() periodically; implementation may emit 0 or more
 * records.
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
  const ProducerStats& stats() const {return stats_; }

  /// @brief Metadata for producers
  struct ProducerMetadata {
    /// @brief Type of the producer
    std::string type;

    /// @brief Unique identifier for the producer
    std::string id;

    /// @brief Human-readable name for the producer
    std::string name;

    /// @brief Description of the producer's function
    std::string description;

    /**
     * @brief Virtual destructor
     */
    virtual ~ProducerMetadata() = default;

    /**
     * @brief Get producer info as JSON
     *
     * Features JSON for use in LeRobot info.json (e.g. action, observation.state, camera features).
     *
     * @return JSON object containing producer information
     */
    virtual nlohmann::ordered_json get_info() const {
      return nlohmann::ordered_json{};
    }

    /**
     * @brief Get per-stream dataset metadata for MCAP recording
     *
     * Returns a JSON object with stream and sensor info for use as MCAP file-level metadata.
     * The object may contain:
     *   - "streams": { "<stream_id>": { "joint_names": [...] } }
     *   - "cameras": { "<camera_id>": { "width", "height", "fps", "channels", ... } }
     *   - "has_mobile_base": true
     *
     * @return JSON object with dataset stream/sensor metadata, or empty if not applicable
     */
    virtual nlohmann::ordered_json get_stream_info() const {
      return nlohmann::ordered_json{};
    }
  };

  /**
   * @brief Get producer metadata
   *
   * @return const reference to ProducerMetadata
   */
  virtual std::shared_ptr<ProducerMetadata> metadata() const = 0;

  /**
   * @brief The hardware component this producer was built from.
   *
   * nullptr for producers with no backing component (e.g. mocks). Set by the
   * ProducerRegistry at creation; the SessionManager uses it to reach the
   * component's per-episode lifecycle hooks.
   *
   * @return Shared pointer to the backing component, or nullptr
   */
  std::shared_ptr<HardwareComponent> hardware() const { return component_; }

  /**
   * @brief Set the backing hardware component. Called by ProducerRegistry::create().
   *
   * @param hw Hardware component this producer was built from
   */
  void set_hardware(std::shared_ptr<HardwareComponent> hw) { component_ = std::move(hw); }

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

  /// @brief Backing hardware component (see hardware()).
  std::shared_ptr<HardwareComponent> component_;
};

/**
 * @brief PushProducer: owns its own internal thread(s); start registers emit callback.
 */
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
   * @brief Get producer metadata
   *
   * @return Shared pointer to ProducerMetadata, or nullptr if not applicable
   */
  virtual std::shared_ptr<PolledProducer::ProducerMetadata> metadata() const { return nullptr; }

  /**
   * @brief Get producer statistics
   *
   * @return const reference to ProducerStats
   */
  const ProducerStats& stats() const {return stats_; }

  /**
   * @brief The hardware component this producer was built from.
   *
   * nullptr for producers with no backing component. Set by the
   * PushProducerRegistry at creation; the SessionManager uses it to reach the
   * component's per-episode lifecycle hooks.
   *
   * @return Shared pointer to the backing component, or nullptr
   */
  std::shared_ptr<HardwareComponent> hardware() const { return component_; }

  /**
   * @brief Set the backing hardware component. Called by PushProducerRegistry::create().
   *
   * @param hw Hardware component this producer was built from
   */
  void set_hardware(std::shared_ptr<HardwareComponent> hw) { component_ = std::move(hw); }

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

  /// @brief Backing hardware component (see hardware()).
  std::shared_ptr<HardwareComponent> component_;
};

}  // namespace trossen::hw

#endif  // TROSSEN_SDK__HW__PRODUCER_BASE_HPP
